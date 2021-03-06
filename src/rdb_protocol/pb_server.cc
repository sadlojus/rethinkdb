// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "rdb_protocol/pb_server.hpp"

#include "errors.hpp"
#include <boost/make_shared.hpp>

#include "concurrency/cross_thread_watchable.hpp"
#include "concurrency/watchable.hpp"
#include "rdb_protocol/stream_cache.hpp"
#include "rpc/semilattice/view/field.hpp"

Response on_unparsable_query2(Query *q, std::string msg) {
    Response res;
    res.set_token( (q && q->has_token()) ? q->token() : -1);
    ql::fill_error(&res, Response::CLIENT_ERROR, msg);
    return res;
}

query2_server_t::query2_server_t(const std::set<ip_address_t> &local_addresses,
                                 int port,
                                 rdb_protocol_t::context_t *_ctx) :
    server(local_addresses, port, boost::bind(&query2_server_t::handle, this, _1, _2),
           &on_unparsable_query2, INLINE),
    ctx(_ctx), parser_id(generate_uuid()), thread_counters(0)
{ }

http_app_t *query2_server_t::get_http_app() {
    return &server;
}

int query2_server_t::get_port() const {
    return server.get_port();
}

Response query2_server_t::handle(Query *q, context_t *query2_context) {
    ql::stream_cache2_t *stream_cache2 = &query2_context->stream_cache2;
    signal_t *interruptor = query2_context->interruptor;
    guarantee(interruptor);
    Response res;
    res.set_token(q->token());

    try {
        boost::shared_ptr<js::runner_t> js_runner = boost::make_shared<js::runner_t>();
        int thread = get_thread_id();
        guarantee(ctx->directory_read_manager);
        scoped_ptr_t<ql::env_t> env(
            new ql::env_t(
                ctx->pool_group, ctx->ns_repo,
                ctx->cross_thread_namespace_watchables[thread]->get_watchable(),
                ctx->cross_thread_database_watchables[thread]->get_watchable(),
                ctx->semilattice_metadata, ctx->directory_read_manager,
                js_runner, interruptor, ctx->machine_id,
                std::map<std::string, ql::wire_func_t>()));
        // `ql::run` will set the status code
        ql::run(q, &env, &res, stream_cache2);
    } catch (const interrupted_exc_t &e) {
        ql::fill_error(&res, Response::RUNTIME_ERROR,
                       "Query interrupted.  Did you shut down the server?");
    } catch (const std::exception &e) {
        ql::fill_error(&res, Response::RUNTIME_ERROR,
                       strprintf("Unexpected exception: %s\n", e.what()));
    }

    return res;
}
