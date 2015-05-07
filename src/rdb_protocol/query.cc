// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/query.hpp"

#include "rdb_protocol/func.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/optargs.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/query_cache.hpp"

namespace ql {

const char *rapidjson_typestr(rapidjson::Type t) {
    switch (t) {
    case rapidjson::kNullType:   return "NULL";
    case rapidjson::kFalseType:  return "BOOL";
    case rapidjson::kTrueType:   return "BOOL";
    case rapidjson::kObjectType: return "OBJECT";
    case rapidjson::kArrayType:  return "ARRAY";
    case rapidjson::kStringType: return "STRING";
    case rapidjson::kNumberType: return "NUMBER";
    default:                     break;
    }
    unreachable();
}

void check_type(const rapidjson::Value &v,
                rapidjson::Type expected_type,
                backtrace_id_t bt) {
    if (v.GetType() != expected_type) {
        throw exc_t(base_exc_t::GENERIC,
            strprintf("Query parse error: expected %s but found %s.",
                      rapidjson_typestr(expected_type),
                      rapidjson_typestr(v.GetType())), bt);
    }
}

void check_term_size(const rapidjson::Value &v, backtrace_id_t bt) {
    if (v.Size() == 0 || v.Size() > 3) {
        throw exc_t(base_exc_t::GENERIC,
            strprintf("Expected an array of 1, 2, or 3 elements, but found %d.",
                      v.Size()), bt);
    }
}

query_params_t::query_id_t::query_id_t(query_params_t::query_id_t &&other) :
        intrusive_list_node_t(std::move(other)),
        parent(other.parent),
        value_(other.value_) {
    parent->assert_thread();
    other.parent = NULL;
}

query_params_t::query_id_t::query_id_t(query_cache_t *_parent) :
        parent(_parent),
        value_(parent->next_query_id++) {
    // Guarantee correct ordering.
    query_id_t *last_newest = parent->outstanding_query_ids.tail();
    guarantee(last_newest == nullptr || last_newest->value() < value_);
    guarantee(value_ >= parent->oldest_outstanding_query_id.get());

    parent->outstanding_query_ids.push_back(this);
}

query_params_t::query_id_t::~query_id_t() {
    if (parent != nullptr) {
        parent->assert_thread();
    } else {
        rassert(!in_a_list());
    }

    if (in_a_list()) {
        parent->outstanding_query_ids.remove(this);
        if (value_ == parent->oldest_outstanding_query_id.get()) {
            query_id_t *next_outstanding_id = parent->outstanding_query_ids.head();
            if (next_outstanding_id == nullptr) {
                parent->oldest_outstanding_query_id.set_value(parent->next_query_id);
            } else {
                guarantee(next_outstanding_id->value() > value_);
                parent->oldest_outstanding_query_id.set_value(next_outstanding_id->value());
            }
        }
    }
}

uint64_t query_params_t::query_id_t::value() const {
    guarantee(in_a_list());
    return value_;
}

query_params_t::query_params_t(int64_t _token,
                               ql::query_cache_t *_query_cache,
                               scoped_array_t<char> &&_original_data,
                               rapidjson::Document &&_query_json) :
        query_cache(_query_cache), query_json(std::move(_query_json)), token(_token),
        id(query_cache), noreply(false), profile(false), root_term_json(nullptr),
        global_optargs_json(nullptr), original_data(std::move(_original_data)) {
    if (!query_json.IsArray()) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("Expected a query to be an array, but found %s.",
                      rapidjson_typestr(query_json.GetType())),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }
    if (query_json.Size() == 0 || query_json.Size() > 3) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("Expected 0 to 3 elements in the top-level query, but found %d.",
                      query_json.Size()),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }

    if (!query_json[0].IsNumber()) {
        throw bt_exc_t(Response::CLIENT_ERROR,
            strprintf("Expected a query type as a number, but found %s.",
                      rapidjson_typestr(query_json[0].GetType())),
            backtrace_registry_t::EMPTY_BACKTRACE);
    }
    type = static_cast<Query::QueryType>(query_json[0].GetInt());

    if (query_json.Size() >= 2) {
        root_term_json = &query_json[1];
    }

    if (query_json.Size() >= 3) {
        if (!query_json[2].IsObject()) {
            throw bt_exc_t(Response::CLIENT_ERROR,
                strprintf("Expected global optargs as an object, but found %s.",
                          rapidjson_typestr(query_json[2].GetType())),
                backtrace_registry_t::EMPTY_BACKTRACE);
        }
        global_optargs_json = &query_json[2];
    }

    // Parse out optargs that are needed before query evaluation
    if (global_optargs_json != nullptr) {
        noreply = static_optarg_as_bool("noreply", noreply);
        profile = static_optarg_as_bool("profile", profile);
    }

    // If the query wants a reply, we can release the query id, which is
    // only used for tracking the ordering of noreply queries for the
    // purpose of noreply_wait.
    if (!noreply) {
        query_id_t destroyer(std::move(id));
    }
}

bool query_params_t::static_optarg_as_bool(const std::string &key, bool default_value) {
    r_sanity_check(global_optargs_json != nullptr);
    auto it = global_optargs_json->FindMember(key.c_str());
    if (it == global_optargs_json->MemberEnd() ||
        !it->value.IsArray() || it->value.Size() != 2 ||
        !it->value[0].IsNumber() ||
        static_cast<Term::TermType>(it->value[0].GetInt()) != Term::DATUM) {
        return default_value;
    }

    datum_t res = to_datum(it->value[1],
                           configured_limits_t::unlimited,
                           reql_version_t::LATEST);

    if (res.has() && res.get_type() == datum_t::type_t::R_BOOL) {
        return res.as_bool();
    }
    return default_value;
}

arg_iterator_t::arg_iterator_t(const intrusive_list_t<raw_term_t> *_list) :
    last_item(nullptr), list(_list) { }

arg_iterator_t::~arg_iterator_t() { }

const raw_term_t *arg_iterator_t::next() {
    if (last_item == nullptr) {
        last_item = list->head();
    } else {
        last_item = list->next(last_item);
    }

    return last_item == nullptr ? last_item :
        (last_item->type == raw_term_t::REFERENCE ?  last_item->src : last_item);
}

optarg_iterator_t::optarg_iterator_t(const intrusive_list_t<raw_term_t> *_list) :
    arg_iterator_t(_list) { }

optarg_iterator_t::~optarg_iterator_t() { }

const std::string &optarg_iterator_t::optarg_name() const {
    guarantee(last_item != nullptr);
    return last_item->optarg_name;
}

const Term::TermType raw_term_t::REFERENCE = static_cast<Term::TermType>(-1);

raw_term_t::raw_term_t() :
    src(nullptr) { }

size_t raw_term_t::num_args() const {
    rassert(type != Term::DATUM);
    if (type == REFERENCE) {
        rassert(src != nullptr && src->type != REFERENCE);
        return src->args_.size();
    }
    return args_.size();
}

size_t raw_term_t::num_optargs() const {
    rassert(type != Term::DATUM);
    if (type == REFERENCE) {
        rassert(src != nullptr && src->type != REFERENCE);
        return src->optargs_.size();
    }
    return optargs_.size();
}

arg_iterator_t raw_term_t::args() const {
    rassert(type != Term::DATUM);
    if (type == REFERENCE) {
        rassert(src != nullptr && src->type != REFERENCE);
        return arg_iterator_t(&src->args_);
    }
    return arg_iterator_t(&args_);
}

optarg_iterator_t raw_term_t::optargs() const {
    rassert(type != Term::DATUM);
    if (type == REFERENCE) {
        rassert(src != nullptr && src->type != REFERENCE);
        return optarg_iterator_t(&src->optargs_);
    }
    return optarg_iterator_t(&optargs_);
}

const datum_t &raw_term_t::datum() const {
    rassert(type == Term::DATUM);
    return value;
}

datum_t &raw_term_t::mutable_datum() {
    rassert(type == Term::DATUM);
    return value;
}

const raw_term_t *&raw_term_t::mutable_ref() {
    rassert(type == REFERENCE);
    return src;
}

intrusive_list_t<raw_term_t> &raw_term_t::mutable_args() {
    rassert(type != Term::DATUM && type != REFERENCE);
    return args_;
}

intrusive_list_t<raw_term_t> &raw_term_t::mutable_optargs() {
    rassert(type != Term::DATUM && type != REFERENCE);
    return optargs_;
}

term_storage_t::term_storage_t() { }

void clear_list(intrusive_list_t<raw_term_t> *list) {
    while (!list->empty()) {
        list->pop_front();
    }
}

// Because intrusive lists cannot be destroyed unless empty, we need to unwind all
// extant raw_term_ts.
term_storage_t::~term_storage_t() {
    clear_list(&global_optarg_list);
    for (size_t i = 0; i < terms.size(); ++i) {
        clear_list(&terms[i].args_);
        clear_list(&terms[i].optargs_);
    }
}

void term_storage_t::add_root_term(const rapidjson::Value &v) {
    parse_internal(v, &backtrace_registry, backtrace_id_t::empty());
}

void term_storage_t::add_global_optargs(const rapidjson::Value &optargs) {
    check_type(optargs, rapidjson::kObjectType, backtrace_id_t::empty());
    bool has_db_optarg = false;
    for (auto it = optargs.MemberBegin(); it != optargs.MemberEnd(); ++it) {
        std::string key(it->name.GetString());
        if (key == "db") {
            has_db_optarg = true;
        }

        minidriver_t r(this, backtrace_id_t::empty());
        raw_term_t *term = parse_internal(it->value, nullptr, backtrace_id_t::empty());

        // Don't do this at home
        const raw_term_t *func_term = r.fun(r.expr(term)).raw_term();
        global_optarg_list.push_back(const_cast<raw_term_t *>(func_term));
        global_optarg_list.tail()->optarg_name = key;
    }

    // Add a default 'test' database optarg if none was specified
    if (!has_db_optarg) {
        minidriver_t r(this, backtrace_id_t::empty());
        const raw_term_t *func_term = r.fun(r.db("test")).raw_term();
        global_optarg_list.push_back(const_cast<raw_term_t *>(func_term));
        global_optarg_list.tail()->optarg_name = std::string("db");
    }
}

datum_t term_storage_t::get_time() {
    if (!start_time.has()) {
        start_time = pseudo::time_now();
    }
    return start_time;
}

raw_term_t *term_storage_t::new_term(Term::TermType type, backtrace_id_t bt) {
    raw_term_t &res = terms.push_back();
    res.type = type;
    res.bt = bt;
    return &res;
}

raw_term_t *term_storage_t::new_ref(const raw_term_t *src) {
    raw_term_t &res = terms.push_back();
    res.type = raw_term_t::REFERENCE;
    res.bt = src->bt;
    if (src->type == raw_term_t::REFERENCE) {
        guarantee(src->src->type != raw_term_t::REFERENCE);
        res.mutable_ref() = src->src;
    } else {
        res.mutable_ref() = src;
    }
    return &res;
}

raw_term_t *term_storage_t::parse_internal(const rapidjson::Value &v,
                                           backtrace_registry_t *bt_reg,
                                           backtrace_id_t bt) {
    raw_term_t *res;
    rapidjson::StringBuffer debug_str;
    rapidjson::Writer<rapidjson::StringBuffer> debug_writer(debug_str);
    v.Accept(debug_writer);
    if (v.IsArray()) {
        debugf("processing term: %s\n", debug_str.GetString());
        check_term_size(v, bt);
        check_type(v[0], rapidjson::kNumberType, bt);
        res = new_term(static_cast<Term::TermType>(v[0].GetInt()), bt);

        if (res->type == Term::DATUM) {
            rcheck_src(bt, v.Size() == 2, base_exc_t::GENERIC,
                       strprintf("Expected 2 items in array, but found %d", v.Size()));
            res->mutable_datum() = to_datum(v[1], configured_limits_t::unlimited,
                                            reql_version_t::LATEST);
        } else if (v.Size() == 2) {
            add_args(v[1], &res->mutable_args(), bt_reg, bt);
        } else if (v.Size() == 3) {
            add_args(v[1], &res->mutable_args(), bt_reg, bt);
            add_optargs(v[2], &res->mutable_optargs(), bt_reg, bt);
        }

        // Convert NOW terms into a literal datum - so they all have the same value
        if (res->type == Term::NOW && res->num_args() == 0 && res->num_optargs() == 0) {
            res->type = Term::DATUM;
            res->mutable_datum() = get_time();
        }
    } else if (v.IsObject()) {
        debugf("converting object to MAKE_OBJ: %s\n", debug_str.GetString());
        res = new_term(Term::MAKE_OBJ, bt);
        add_optargs(v, &res->mutable_optargs(), bt_reg, bt);
    } else {
        debugf("converting json to datum: %s\n", debug_str.GetString());
        res = new_term(Term::DATUM, bt);
        res->mutable_datum() = to_datum(v, configured_limits_t::unlimited,
                                        reql_version_t::LATEST);
    }
    return res;
}

void term_storage_t::add_args(const rapidjson::Value &args,
                              intrusive_list_t<raw_term_t> *args_out,
                              backtrace_registry_t *bt_reg,
                              backtrace_id_t bt) {
    check_type(args, rapidjson::kArrayType, bt);
    for (size_t i = 0; i < args.Size(); ++i) {
        backtrace_id_t child_bt = (bt_reg == NULL) ?
            backtrace_id_t::empty() :
            bt_reg->new_frame(bt, ql::datum_t(static_cast<double>(i)));
        raw_term_t *t = parse_internal(args[i], bt_reg, child_bt);
        args_out->push_back(t);
    }
}

void term_storage_t::add_optargs(const rapidjson::Value &optargs,
                                 intrusive_list_t<raw_term_t> *optargs_out,
                                 backtrace_registry_t *bt_reg,
                                 backtrace_id_t bt) {
    check_type(optargs, rapidjson::kObjectType, bt);
    for (auto it = optargs.MemberBegin(); it != optargs.MemberEnd(); ++it) {
        backtrace_id_t child_bt = (bt_reg == NULL) ?
            backtrace_id_t::empty() :
            bt_reg->new_frame(bt,
                ql::datum_t(datum_string_t(it->name.GetStringLength(),
                                           it->name.GetString())));
        raw_term_t *t = parse_internal(it->value, bt_reg, child_bt);
        optargs_out->push_back(t);
        t->optarg_name = std::string(it->name.GetString());
    }
}

template <cluster_version_t W>
archive_result_t term_storage_t::deserialize_term_tree(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version) {
    CT_ASSERT(sizeof(int) == sizeof(int32_t));
    int32_t size;
    archive_result_t res = deserialize_universal(s, &size);
    if (bad(res)) { return res; }
    if (size < 0) { return archive_result_t::RANGE_ERROR; }
    scoped_array_t<char> data(size);
    int64_t read_res = force_read(s, data.data(), data.size());
    if (read_res != size) { return archive_result_t::SOCK_ERROR; }
    Term t;
    t.ParseFromArray(data.data(), data.size());
    *term_out = parse_internal(t, reql_version);
    return archive_result_t::SUCCESS;
}

raw_term_t *term_storage_t::parse_internal(const Term &term,
                                           reql_version_t reql_version) {
    r_sanity_check(term.has_type());
    raw_term_t *raw_term = new_term(term.type(), backtrace_id_t::empty());

    if (term.type() == Term::DATUM) {
        raw_term->mutable_datum() =
            to_datum(&term.datum(), configured_limits_t::unlimited, reql_version);
    } else {
        for (int i = 0; i < term.args_size(); ++i) {
            raw_term->mutable_args().push_back(parse_internal(term.args(i),
                                                              reql_version));
        }
        for (int i = 0; i < term.optargs_size(); ++i) {
            const Term_AssocPair &optarg_term = term.optargs(i);
            raw_term_t *optarg = parse_internal(optarg_term.val(), reql_version);
            optarg->optarg_name = optarg_term.key();
            raw_term->mutable_optargs().push_back(optarg);
        }
    }
    return raw_term;
}

template
archive_result_t term_storage_t::deserialize_term_tree<cluster_version_t::v1_14>(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version);
template
archive_result_t term_storage_t::deserialize_term_tree<cluster_version_t::v1_15>(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version);
template
archive_result_t term_storage_t::deserialize_term_tree<cluster_version_t::v1_16>(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version);
template
archive_result_t term_storage_t::deserialize_term_tree<cluster_version_t::v2_0>(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version);

template <>
archive_result_t
term_storage_t::deserialize_term_tree<cluster_version_t::v2_1_is_latest>(
        read_stream_t *s, raw_term_t **term_out, reql_version_t reql_version) {
    const cluster_version_t W = cluster_version_t::v2_1_is_latest;
    archive_result_t res;

    int32_t type;
    backtrace_id_t bt;
    res = deserialize<W>(s, &type);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &bt);
    if (bad(res)) { return res; }
    raw_term_t *term = new_term(static_cast<Term::TermType>(type), bt);

    if (term->type == Term::DATUM) {
        deserialize<W>(s, &term->mutable_datum());
    } else {
        size_t num_args;
        res = deserialize<W>(s, &num_args);
        if (bad(res)) { return res; }
        for (size_t i = 0; i < num_args; ++i) {
            res = deserialize_term_tree<W>(s, term_out, reql_version);
            if (bad(res)) { return res; }
            term->mutable_args().push_back(*term_out);
        }
        std::string optarg_name;
        size_t num_optargs;
        res = deserialize<W>(s, &num_optargs);
        if (bad(res)) { return res; }
        for (size_t i = 0; i < num_optargs; ++i) {
            res = deserialize<W>(s, &optarg_name);
            if (bad(res)) { return res; }
            res = deserialize_term_tree<W>(s, term_out, reql_version);
            if (bad(res)) { return res; }
            (*term_out)->optarg_name = optarg_name;
            term->mutable_optargs().push_back(*term_out);
        }
    }
    *term_out = term;
    return res;
}

template <cluster_version_t W>
void serialize_term_tree(write_message_t *wm,
                         const raw_term_t *term) {
    serialize<W>(wm, static_cast<int32_t>(term->type));
    serialize<W>(wm, term->bt);
    if (term->type == Term::DATUM) {
        serialize<W>(wm, term->datum());
    } else {
        size_t num_args = term->num_args();
        serialize<W>(wm, num_args);
        auto arg_it = term->args();
        while (const raw_term_t *t = arg_it.next()) {
            serialize_term_tree<W>(wm, t);
            --num_args;
        }
        std::string optarg_name;
        size_t num_optargs = term->num_optargs();
        serialize<W>(wm, num_optargs);
        auto optarg_it = term->optargs();
        while (const raw_term_t *t = optarg_it.next()) {
            optarg_name.assign(optarg_it.optarg_name());
            serialize<W>(wm, optarg_name);
            serialize_term_tree<W>(wm, t);
            --num_optargs;
        }
        r_sanity_check(num_args == 0);
        r_sanity_check(num_optargs == 0);
    }
}

template
void serialize_term_tree<cluster_version_t::CLUSTER>(
    write_message_t *wm, const raw_term_t *term);

} // namespace ql