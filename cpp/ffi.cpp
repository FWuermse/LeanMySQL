#include <lean/lean.h>
#include <mysql/mysql.h>
#include <mysql/field_types.h>
#include <string.h>
#include <stdio.h>

#define internal inline static
#define external extern "C"
#define l_arg b_lean_obj_arg
#define l_res lean_obj_res

const char* ERR_INCR_BFFR = "Not enough memory. Try increasing the buffer size.";

typedef struct mysql {
    MYSQL* connection = NULL;
    char       logged = 0;
    int        status = 0;
    int   buffer_size = 0;
    int    buffer_pos = 0;
    char*      buffer = NULL;
    char   has_result = 0;
    MYSQL_RES* result = NULL;
} mysql;

MYSQL_ROW row;

static lean_external_class* g_mysql_external_class = NULL;

internal lean_object* mysql_box(mysql* m) {
    return lean_alloc_external(g_mysql_external_class, m);
}

internal mysql* mysql_unbox(lean_object* p) {
    return (mysql*) (lean_get_external_data(p));
}

internal l_res make_error(const char* err_msg) {
    return lean_mk_io_user_error(lean_mk_io_user_error(lean_mk_string(err_msg)));
}

internal void close_connection(mysql* m) {
    mysql_close(m->connection);
    m->logged = 0;
    if (m->result) {
        free(m->result);
    }
}

internal void mysql_finalizer(void* mysql_ptr) {
    mysql* m = (mysql*) mysql_ptr;
    close_connection(m);
    if (m->connection) {
        free(m->connection);
    }
    free(m->buffer);
    free(m);
}

internal void noop_foreach(void* mod, l_arg fn) {}

internal void query_all(mysql* m, const char* q) {
    m->status = mysql_query(m->connection, q);
    m->result = mysql_store_result(m->connection);
}

internal void query_some(mysql* m, const char* q) {
    m->status = mysql_query(m->connection, q);
    m->result = mysql_use_result(m->connection);
}

internal l_res lean_mysql_manage_db(l_arg m_, l_arg q_) {
    mysql* m = mysql_unbox(m_);
    if (!m->logged) {
        return make_error("Not logged in.");
    }

    query_all(m, lean_string_cstr(q_));
    if (m->status) {
        return make_error(mysql_error(m->connection));
    }
    return lean_io_result_mk_ok(lean_box(0));
}

internal char append_to_buffer(mysql* m, const char* s) {
    int size = strlen(s);
    if (m->buffer_size - m->buffer_pos < size + 1) {
        return 0;
    }
    memcpy(m->buffer + m->buffer_pos, s, size + 1);
    m->buffer_pos = m->buffer_pos + size;
    return 1;
}

internal const char* type_to_str(int t) {
    switch (t) {
        case MYSQL_TYPE_TINY:
            return "nat";
        case MYSQL_TYPE_SHORT:
            return "nat";
        case MYSQL_TYPE_LONG:
            return "nat";
        case MYSQL_TYPE_LONGLONG:
            return "nat";
        case MYSQL_TYPE_INT24:
            return "nat";
        case MYSQL_TYPE_DECIMAL:
            return "float";
        case MYSQL_TYPE_FLOAT:
            return "float";
        case MYSQL_TYPE_DOUBLE:
            return "float";
        default:
            return "string";
    }
}

// API

external l_res lean_mysql_initialize() {
    g_mysql_external_class = lean_register_external_class(mysql_finalizer, noop_foreach);
    return lean_io_result_mk_ok(lean_box(0));
}

external l_res lean_mysql_mk(uint32_t b) {
    mysql* m = (mysql*) malloc(sizeof(mysql));
    int size = (b - 1) * 1024;
    char* buffer = (char*)malloc(size);
    if (buffer == NULL) {
        return make_error("Not enough memory to allocate buffer.");
    }
    m->buffer = buffer;
    m->buffer_size = size;
    return lean_io_result_mk_ok(mysql_box(m));
}

external l_res lean_mysql_set_buffer_size(l_arg m_, uint32_t b) {
    mysql* m = mysql_unbox(m_);
    int size = (b - 1) * 1024;
    char* buffer = (char*)malloc(size);
    if (buffer == NULL) {
        return make_error("Not enough memory to allocate buffer.");
    }
    free(m->buffer);
    m->buffer = buffer;
    m->buffer_size = size;
    return lean_io_result_mk_ok(lean_box(0));
}

external l_res lean_mysql_version() {
    return lean_io_result_mk_ok(lean_mk_string(mysql_get_client_info()));
}

external l_res lean_mysql_login(l_arg m_, l_arg h_, l_arg u_, l_arg p_) {
    mysql* m = mysql_unbox(m_);
    if (m->logged) {
        return make_error("Already logged in. Try using 'close' first.");
    }

    m->connection = mysql_init(NULL);
    if (m->connection == NULL) {
        return make_error("Failed to instantiate a connection with MySQL.");
    }

    const char* h = lean_string_cstr(h_);
    const char* u = lean_string_cstr(u_);
    const char* p = lean_string_cstr(p_);
    MYSQL *connection_ret = mysql_real_connect(
        m->connection,
        h, u, p,
        NULL, 0, NULL, 0
    );

    if (connection_ret == NULL) {
        return make_error(mysql_error(m->connection));
    }
    else {
        m->logged = 1;
        return lean_io_result_mk_ok(lean_box(0));
    }
}

external l_res lean_mysql_run(l_arg m_, l_arg q_) {
    return lean_mysql_manage_db(m_, q_);
}

external l_res lean_mysql_query(l_arg m_, l_arg q_) {
    mysql* m = mysql_unbox(m_);
    if (!m->logged) {
        return make_error("Not logged in.");
    }

    query_all(m, lean_string_cstr(q_));
    if (m->status) {
        return make_error(mysql_error(m->connection));
    }

    int num_fields = mysql_num_fields(m->result);
    MYSQL_FIELD* field;
    MYSQL_ROW row;

    m->buffer_pos = 0;
    m->has_result = 0;

    for(int i = 0; i < num_fields; i++) {
        field = mysql_fetch_field(m->result);
        if (!append_to_buffer(m, field->name)) {
            return make_error(ERR_INCR_BFFR);
        }
        if (!append_to_buffer(m, " ")) {
            return make_error(ERR_INCR_BFFR);
        }
        if (!append_to_buffer(m, type_to_str(field->type))) {
            return make_error(ERR_INCR_BFFR);
        }
        if (i < num_fields - 1) {
            if (!append_to_buffer(m, "~")) {
                return make_error(ERR_INCR_BFFR);
            }
        }
    }
    if (!append_to_buffer(m, "¨")) {
        return make_error(ERR_INCR_BFFR);
    }

    while (row = mysql_fetch_row(m->result)) {
        for(int i = 0; i < num_fields; i++) {
            if (!append_to_buffer(m, row[i] ? row[i] : "NULL")) {
                return make_error(ERR_INCR_BFFR);
            }
            if (i < num_fields - 1) {
                if (!append_to_buffer(m, "~")) {
                    return make_error(ERR_INCR_BFFR);
                }
            }
        }
        if (!append_to_buffer(m, "¨")) {
            return make_error(ERR_INCR_BFFR);
        }
    }

    if (!append_to_buffer(m, "\0")) {
        return make_error(ERR_INCR_BFFR);
    }

    m->has_result = 1;

    return lean_io_result_mk_ok(lean_box(0));
}

external l_res lean_mysql_get_query_result(l_arg m_) {
    mysql* m = mysql_unbox(m_);
    if (!m->has_result) {
        return lean_io_result_mk_ok(lean_mk_string(""));
    }
    return lean_mk_string(m->buffer);
}

external l_res lean_mysql_close(l_arg m_) {
    mysql* m = mysql_unbox(m_);
    close_connection(m);
    return lean_io_result_mk_ok(lean_box(0));
}
