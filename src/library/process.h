/*
Copyright (c) 2017 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Jared Roesch
*/
#pragma once

#include <iostream>
#include <string>
#include "library/handle.h"
#include "util/buffer.h"
#include "library/pipe.h"

namespace lean  {

enum stdio {
    INHERIT,
    PIPED,
    NUL,
};

struct child {
    handle_ref m_stdin;
    handle_ref m_stdout;
    handle_ref m_stderr;
    int m_pid;

    child(child const & ch) :
        m_stdin(ch.m_stdin),
        m_stdout(ch.m_stdout),
        m_stderr(ch.m_stderr),
        m_pid(ch.m_pid) {}

    child(int pid, handle_ref hstdin, handle_ref hstdout, handle_ref hstderr) :
        m_stdin(hstdin),
        m_stdout(hstdout),
        m_stderr(hstderr),
        m_pid(pid) {}

    unsigned wait();
};

class process {
    std::string m_proc_name;
    buffer<std::string> m_args;
    optional<stdio> m_stdout;
    optional<stdio> m_stdin;
    optional<stdio> m_stderr;
    optional<std::string> m_cwd;
public:
    process(process const & proc) = default;
    process(std::string exe_name);
    process & arg(std::string arg_str);
    process & set_stdin(stdio cfg);
    process & set_stdout(stdio cfg);
    process & set_stderr(stdio cfg);
    process & set_cwd(std::string const & cwd);
    child spawn();
    void run();
};
}
