#include "rpc/server.h"

#include "likely.h"
#include "prometheus/prometheus_sanitize.h"
#include "rpc/logger.h"
#include "rpc/parse_utils.h"
#include "rpc/types.h"

#include <seastar/core/metrics.hh>

#include <fmt/format.h>

namespace rpc {
struct server_context_impl final : streaming_context {
    server_context_impl(server& s, header h)
      : _s(std::ref(s))
      , _h(std::move(h)) {}
    ss::future<ss::semaphore_units<>> reserve_memory(size_t ask) final {
        auto fut = get_units(_s.get()._memory, ask);
        if (_s.get()._memory.waiters()) {
            _s.get()._probe.waiting_for_available_memory();
        }
        return fut;
    }
    const header& get_header() const final { return _h; }
    void signal_body_parse() final { pr.set_value(); }
    std::reference_wrapper<server> _s;
    header _h;
    ss::promise<> pr;
};

server::server(server_configuration c)
  : cfg(std::move(c))
  , _memory(cfg.max_service_memory_per_core)
  , _creds(
      cfg.credentials ? (*cfg.credentials).build_server_credentials()
                      : nullptr) {
    if (!cfg.disable_metrics) {
        setup_metrics();
        _probe.setup_metrics(_metrics);
    }
}

server::~server() {}

void server::start() {
    for (auto addr : cfg.addrs) {
        ss::server_socket ss;
        try {
            ss::listen_options lo;
            lo.reuse_address = true;
            if (!_creds) {
                ss = ss::engine().listen(addr, lo);
            } else {
                ss = ss::tls::listen(_creds, ss::engine().listen(addr, lo));
            }
        } catch (...) {
            throw std::runtime_error(fmt::format(
              "Error attempting to listen on {}: {}",
              addr,
              std::current_exception()));
        }
        _listeners.emplace_back(std::move(ss));
        ss::server_socket& ref = _listeners.back();
        // background
        (void)with_gate(_conn_gate, [this, &ref] { return accept(ref); });
    }
}

ss::future<> server::accept(ss::server_socket& s) {
    return ss::repeat([this, &s]() mutable {
        return s.accept().then_wrapped(
          [this](ss::future<ss::accept_result> f_cs_sa) mutable {
              if (_as.abort_requested()) {
                  f_cs_sa.ignore_ready_future();
                  return ss::make_ready_future<ss::stop_iteration>(
                    ss::stop_iteration::yes);
              }
              auto [ar] = f_cs_sa.get();
              ar.connection.set_nodelay(true);
              ar.connection.set_keepalive(true);
              auto conn = ss::make_lw_shared<connection>(
                _connections,
                std::move(ar.connection),
                std::move(ar.remote_address),
                _probe);
              rpclog.trace("Incoming connection from {}", ar.remote_address);
              if (_conn_gate.is_closed()) {
                  return conn->shutdown().then([] {
                      return ss::make_exception_future<ss::stop_iteration>(
                        ss::gate_closed_exception());
                  });
              }
              (void)with_gate(_conn_gate, [this, conn]() mutable {
                  return continous_method_dispath(conn)
                    .then_wrapped([conn](ss::future<>&& f) {
                        rpclog.debug("closing client: {}", conn->addr);
                        return conn->shutdown()
                          .then([f = std::move(f)]() mutable {
                              try {
                                  f.get();
                              } catch (...) {
                                  rpclog.error(
                                    "Error dispatching method: {}",
                                    std::current_exception());
                              }
                          })
                          .finally([conn] {});
                    })
                    .finally([conn] {});
              });
              return ss::make_ready_future<ss::stop_iteration>(
                ss::stop_iteration::no);
          });
    });
}

ss::future<>
server::continous_method_dispath(ss::lw_shared_ptr<connection> conn) {
    return ss::do_until(
      [this, conn] { return conn->input().eof() || _as.abort_requested(); },
      [this, conn] {
          return parse_header(conn->input())
            .then([this, conn](std::optional<header> h) {
                if (!h) {
                    rpclog.debug(
                      "could not parse header from client: {}", conn->addr);
                    _probe.header_corrupted();
                    return ss::make_ready_future<>();
                }
                return dispatch_method_once(std::move(h.value()), conn);
            });
      });
}

ss::future<>
server::dispatch_method_once(header h, ss::lw_shared_ptr<connection> conn) {
    const auto method_id = h.meta;
    constexpr size_t header_size = sizeof(header);
    auto ctx = ss::make_lw_shared<server_context_impl>(*this, std::move(h));
    auto it = std::find_if(
      _services.begin(),
      _services.end(),
      [method_id](std::unique_ptr<service>& srvc) {
          return srvc->method_from_id(method_id) != nullptr;
      });
    if (unlikely(it == _services.end())) {
        _probe.method_not_found();
        throw std::runtime_error(
          fmt::format("received invalid rpc request: {}", h));
    }
    auto fut = ctx->pr.get_future();
    method* m = it->get()->method_from_id(method_id);
    _probe.add_bytes_received(header_size + h.size);
    // background!
    if (_conn_gate.is_closed()) {
        return ss::make_exception_future<>(ss::gate_closed_exception());
    }
    (void)with_gate(_conn_gate, [this, conn, ctx, m]() mutable {
        return (*m)(conn->input(), *ctx)
          .then([this, ctx, conn, m = _hist.auto_measure()](netbuf n) mutable {
              n.set_correlation_id(ctx->get_header().correlation_id);
              auto view = std::move(n).as_scattered();
              if (_conn_gate.is_closed()) {
                  // do not write if gate is closed
                  rpclog.debug(
                    "Skipping write of {} bytes, connection is closed",
                    view.size());
                  return ss::make_ready_future<>();
              }
              return conn->write(std::move(view))
                .finally([m = std::move(m), ctx] {});
          })
          .finally([&p = _probe, conn] { p.request_completed(); });
    });
    return fut;
}
ss::future<> server::stop() {
    rpclog.info("Stopping {} listeners", _listeners.size());
    for (auto&& l : _listeners) {
        l.abort_accept();
    }
    rpclog.debug("Service probes {}", _probe);
    rpclog.info("Shutting down {} connections", _connections.size());
    _as.request_abort();
    // close the connections and wait for all dispatches to finish
    for (auto& c : _connections) {
        c.shutdown_input();
    }
    return _conn_gate.close().then([this] {
        return seastar::do_for_each(
          _connections, [](connection& c) { return c.shutdown(); });
    });
}
void server::setup_metrics() {
    namespace sm = ss::metrics;
    _metrics.add_group(
      prometheus_sanitize::metrics_name("rpc"),
      {sm::make_gauge(
         "services",
         [this] { return _services.size(); },
         sm::description("Number of registered services")),
       sm::make_gauge(
         "max_service_mem",
         [this] { return cfg.max_service_memory_per_core; },
         sm::description("Maximum amount of memory used by service per core")),
       sm::make_gauge(
         "consumed_mem",
         [this] { return cfg.max_service_memory_per_core - _memory.current(); },
         sm::description("Amount of memory consumed for requests processing")),
       sm::make_histogram(
         "dispatch_handler_latency",
         [this] { return _hist.seastar_histogram_logform(); },
         sm::description("Latency of service handler dispatch"))});
}
} // namespace rpc
