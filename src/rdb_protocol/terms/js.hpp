#ifndef RDB_PROTOCOL_TERMS_JS_HPP_
#define RDB_PROTOCOL_TERMS_JS_HPP_

#include <string>

#include "errors.hpp"
#include <boost/variant/static_visitor.hpp>

#include "rdb_protocol/op.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/js.hpp"

namespace ql {

static const char *const js_optargs[] = {"timeout"};
class javascript_term_t : public op_term_t {
public:
    javascript_term_t(env_t *env, const Term *term)
        : op_term_t(env, term, argspec_t(1), optargspec_t(js_optargs)) { }
private:

    virtual val_t *eval_impl() {
        std::string source = arg(0)->as_datum()->as_str();

        boost::shared_ptr<js::runner_t> js = env->get_js_runner();

        // Optarg seems designed to take a default vaule as the second argument
        // but nowhere else is this actually used.
        double timeout_s = 5.0;
        val_t *timeout_opt = optarg("timeout", NULL);
        if (timeout_opt) {
            timeout_s = timeout_opt->as_num();
        }

        // JS runner configuration is limited to setting an execution timeout.
        js::runner_t::req_config_t config;
        config.timeout_ms = timeout_s * 1000;

        try {
            js::js_result_t result = js->eval(source, &config);
            return boost::apply_visitor(js_result_visitor_t(env, this), result);
        } catch (const interrupted_exc_t &e) {
            rfail("JavaScript query \"%s\" timed out after %.2G seconds", source.c_str(), timeout_s);
        }

    }
    virtual const char *name() const { return "javascript"; }

    // No JS term is considered deterministic
    virtual bool is_deterministic_impl() const {
        return false;
    }
};

}

#endif // RDB_PROTOCOL_TERMS_JS_HPP_
