/*!The Treasure Box Library
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * Copyright (C) 2009-2020, TBOOX Open Source Group.
 *
 * @author      ruki
 * @file        process.c
 * @ingroup     platform
 */

/* //////////////////////////////////////////////////////////////////////////////////////
 * includes
 */
#include "prefix.h"
#include "../file.h"
#include "../path.h"
#include "../time.h"
#include "../process.h"
#include "../environment.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef TB_CONFIG_POSIX_HAVE_POSIX_SPAWNP
#   include <spawn.h>
#endif
#ifdef TB_CONFIG_LIBC_HAVE_KILL
#   include <signal.h>
#   include <sys/types.h>
#endif
#if defined(TB_CONFIG_MODULE_HAVE_COROUTINE) \
        && !defined(TB_CONFIG_MICRO_ENABLE)
#   include "../../coroutine/coroutine.h"
#   include "../../coroutine/impl/impl.h"
#endif
/* //////////////////////////////////////////////////////////////////////////////////////
 * macros
 */

// pipe file to fd
#define tb_pipefile2fd(file)            (tb_int_t)((file)? (((tb_long_t)(file)) - 1) : -1)

/* //////////////////////////////////////////////////////////////////////////////////////
 * types
 */

// the process type
typedef struct __tb_process_t
{
    // the pid
    pid_t                       pid;

    // the user private data
    tb_cpointer_t               priv;

#ifdef TB_CONFIG_POSIX_HAVE_POSIX_SPAWNP
    // the spawn attributes
    posix_spawnattr_t           spawn_attr;

    // the spawn action
    posix_spawn_file_actions_t  spawn_action;
#endif

}tb_process_t; 

/* //////////////////////////////////////////////////////////////////////////////////////
 * globals
 */

// the user environment
extern tb_char_t**  environ;

/* //////////////////////////////////////////////////////////////////////////////////////
 * declaration
 */
__tb_extern_c_enter__
tb_int_t tb_process_pid(tb_process_ref_t self);
__tb_extern_c_leave__

/* //////////////////////////////////////////////////////////////////////////////////////
 * private implementation
 */
static tb_int_t tb_process_file_flags(tb_size_t mode)
{
    // no mode? uses the default mode
    if (!mode) mode = TB_FILE_MODE_RW | TB_FILE_MODE_CREAT | TB_FILE_MODE_TRUNC;

    // make flags
    tb_size_t flags = 0;
    if (mode & TB_FILE_MODE_RO)         flags |= O_RDONLY;
    else if (mode & TB_FILE_MODE_WO)    flags |= O_WRONLY;
    else if (mode & TB_FILE_MODE_RW)    flags |= O_RDWR;
    if (mode & TB_FILE_MODE_CREAT)      flags |= O_CREAT;
    if (mode & TB_FILE_MODE_APPEND)     flags |= O_APPEND;
    if (mode & TB_FILE_MODE_TRUNC)      flags |= O_TRUNC;

    // ok?
    return flags;
}
static tb_int_t tb_process_file_modes(tb_size_t mode)
{
    // no mode? uses the default mode
    if (!mode) mode = TB_FILE_MODE_RW | TB_FILE_MODE_CREAT | TB_FILE_MODE_TRUNC;

    // make modes
    tb_size_t modes = 0;
    if (mode & TB_FILE_MODE_CREAT) modes = 0777;

    // ok?
    return modes;
}
tb_int_t tb_process_pid(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return_val(process, -1);

    return (tb_int_t)process->pid;
}

/* //////////////////////////////////////////////////////////////////////////////////////
 * implementation
 */
#if defined(TB_CONFIG_POSIX_HAVE_POSIX_SPAWNP)
tb_process_ref_t tb_process_init(tb_char_t const* pathname, tb_char_t const* argv[], tb_process_attr_ref_t attr)
{
    // check
    tb_assert_and_check_return_val(pathname, tb_null);

    // done
    tb_bool_t       ok = tb_false;
    tb_process_t*   process = tb_null;
    do
    {
        // make process
        process = tb_malloc0_type(tb_process_t);
        tb_assert_and_check_break(process);

        // save the user private data
        if (attr) process->priv = attr->priv;

        // init spawn attributes
        posix_spawnattr_init(&process->spawn_attr);

        // init spawn action
        posix_spawn_file_actions_init(&process->spawn_action);

        // set attributes
        if (attr)
        {
            // redirect the stdout
            if (attr->outtype == TB_PROCESS_REDIRECT_TYPE_FILEPATH && attr->outpath)
            {
                // open stdout
                tb_int_t result = posix_spawn_file_actions_addopen(&process->spawn_action, STDOUT_FILENO, attr->outpath, tb_process_file_flags(attr->outmode), tb_process_file_modes(attr->outmode));
                tb_assertf_pass_and_check_break(!result, "cannot redirect stdout to file: %s, error: %d", attr->outpath, result);
            }
            else if ((attr->outtype == TB_PROCESS_REDIRECT_TYPE_PIPE && attr->outpipe) ||
                     (attr->outtype == TB_PROCESS_REDIRECT_TYPE_FILE && attr->outfile))
            {
                // duplicate outpipe/file fd to stdout in the child process
                tb_int_t outfd = attr->outtype == TB_PROCESS_REDIRECT_TYPE_PIPE? tb_pipefile2fd(attr->outpipe) : tb_file2fd(attr->outfile);
                posix_spawn_file_actions_adddup2(&process->spawn_action, outfd, STDOUT_FILENO);
                posix_spawn_file_actions_addclose(&process->spawn_action, outfd);
            }

            // redirect the stderr
            if (attr->errtype == TB_PROCESS_REDIRECT_TYPE_FILEPATH && attr->errpath)
            {
                // open stderr
                tb_int_t result = posix_spawn_file_actions_addopen(&process->spawn_action, STDERR_FILENO, attr->errpath, tb_process_file_flags(attr->errmode), tb_process_file_modes(attr->errmode));
                tb_assertf_pass_and_check_break(!result, "cannot redirect stderr to file: %s, error: %d", attr->errpath, result);
            }
            else if ((attr->errtype == TB_PROCESS_REDIRECT_TYPE_PIPE && attr->errpipe) ||
                     (attr->errtype == TB_PROCESS_REDIRECT_TYPE_FILE && attr->errfile))
            {
                // duplicate errpipe/file fd to stderr in the child process
                tb_int_t errfd = attr->errtype == TB_PROCESS_REDIRECT_TYPE_PIPE? tb_pipefile2fd(attr->errpipe) : tb_file2fd(attr->errfile);
                posix_spawn_file_actions_adddup2(&process->spawn_action, errfd, STDERR_FILENO);
                posix_spawn_file_actions_addclose(&process->spawn_action, errfd);
            }

            // suspend it first
            if (attr->flags & TB_PROCESS_FLAG_SUSPEND)
            {
#ifdef POSIX_SPAWN_START_SUSPENDED
                posix_spawnattr_setflags(&process->spawn_attr, POSIX_SPAWN_START_SUSPENDED);
#else
                tb_assertf(0, "suspend process not supported!");
#endif
            }
        }

        // no given environment? uses the current user environment
        tb_char_t const** envp = attr? attr->envp : tb_null;
        if (!envp) envp = (tb_char_t const**)environ;

        // spawn the process
        tb_int_t status = posix_spawnp(&process->pid, pathname, &process->spawn_action, &process->spawn_attr, (tb_char_t* const*)argv, (tb_char_t* const*)envp);
        if (status != 0)
        {
            // pass the error code to parent/errno 
            errno = status;
            break;
        }

        // check pid
        tb_assert_and_check_break(process->pid > 0);

        // ok
        ok = tb_true;

    } while (0);

    // failed?
    if (!ok)
    {
        // exit it
        if (process) tb_process_exit((tb_process_ref_t)process);
        process = tb_null;
    }

    // ok?
    return (tb_process_ref_t)process;
}
#else
tb_process_ref_t tb_process_init(tb_char_t const* pathname, tb_char_t const* argv[], tb_process_attr_ref_t attr)
{
    // check
    tb_assert_and_check_return_val(pathname, tb_null);

    // done
    tb_bool_t       ok = tb_false;
    tb_process_t*   process = tb_null;
    do
    {
        // make process
        process = tb_malloc0_type(tb_process_t);
        tb_assert_and_check_break(process);

        // save the user private data
        if (attr) process->priv = attr->priv;

        // fork it
#if defined(TB_CONFIG_POSIX_HAVE_VFORK) && \
        defined(TB_CONFIG_POSIX_HAVE_EXECVPE)
        switch ((process->pid = vfork()))
#else
        switch ((process->pid = fork()))
#endif
        {
        case -1:

            // trace
            tb_trace_e("fork failed!");

            // exit it
            _exit(-1);

        case 0: 

            // TODO
            // check
            tb_assertf(!attr || !(attr->flags & TB_PROCESS_FLAG_SUSPEND), "suspend process not supported!");

            // set attributes
            if (attr)
            {
                // redirect the stdout
                if (attr->outtype == TB_PROCESS_REDIRECT_TYPE_FILEPATH && attr->outpath)
                {
                    // open file
                    tb_int_t outfd = open(attr->outpath, tb_process_file_flags(attr->outmode), tb_process_file_modes(attr->outmode));
                    tb_assertf_pass_and_check_break(outfd, "cannot redirect stdout to file: %s, error: %d", attr->outpath, errno);

                    // redirect it
                    dup2(outfd, STDOUT_FILENO);
                    close(outfd);
                }
                else if ((attr->outtype == TB_PROCESS_REDIRECT_TYPE_PIPE && attr->outpipe) ||
                         (attr->outtype == TB_PROCESS_REDIRECT_TYPE_FILE && attr->outfile))
                {
                    // duplicate outpipe fd to stdout in the child process
                    tb_int_t outfd = attr->outtype == TB_PROCESS_REDIRECT_TYPE_PIPE? tb_pipefile2fd(attr->outpipe) : tb_file2fd(attr->outfile);
                    dup2(outfd, STDOUT_FILENO);
                    close(outfd);
                }

                // redirect the stderr
                if (attr->errtype == TB_PROCESS_REDIRECT_TYPE_FILEPATH && attr->errpath)
                {
                    // open file
                    tb_int_t errfd = open(attr->errpath, tb_process_file_flags(attr->errmode), tb_process_file_modes(attr->errmode));
                    tb_assertf_pass_and_check_break(errfd, "cannot redirect stderr to file: %s, error: %d", attr->errpath, errno);

                    // redirect it
                    dup2(errfd, STDERR_FILENO);
                    close(errfd);
                }
                else if ((attr->errtype == TB_PROCESS_REDIRECT_TYPE_PIPE && attr->errpipe) ||
                         (attr->errtype == TB_PROCESS_REDIRECT_TYPE_FILE && attr->errfile))
                {
                    // duplicate errpipe fd to stderr in the child process
                    tb_int_t errfd = attr->errtype == TB_PROCESS_REDIRECT_TYPE_PIPE? tb_pipefile2fd(attr->errpipe) : tb_file2fd(attr->errfile);
                    dup2(errfd, STDERR_FILENO);
                    close(errfd);
                }
            }

            // get environment 
            tb_char_t const** envp = attr? attr->envp : tb_null;

#if defined(TB_CONFIG_POSIX_HAVE_EXECVPE)
            // no given environment? uses the current user environment
            if (!envp) envp = (tb_char_t const**)environ;

            // exec it in the child process
            execvpe(pathname, (tb_char_t* const*)argv, (tb_char_t* const*)envp);
#elif defined(TB_CONFIG_POSIX_HAVE_EXECVP)

            /* set environment variables
             *
             * uses fork because it will modify the parent environment
             */
            if (envp)
            {
                // done
                tb_char_t const* env = tb_null;
                while ((env = *envp++))
                {
                    // get name and values
                    tb_char_t const* p = tb_strchr(env, '=');
                    if (p)
                    {
                        // get name
                        tb_char_t name[256];
                        tb_size_t size = tb_min(p - env, sizeof(name) - 1);
                        tb_strncpy(name, env, size);
                        name[size] = '\0';

                        // get values
                        tb_char_t const* values = p + 1;

                        // set values to the environment
                        tb_environment_set(name, values);
                    }
                }
            }

            // exec it in the child process
            execvp(pathname, (tb_char_t* const*)argv);
#else
#   error 
#endif
            // exit it
            _exit(-1);

        default:
            // parent
            break;
        }

        // check pid
        tb_assert_and_check_break(process->pid > 0);

        // ok
        ok = tb_true;

    } while (0);

    // failed?
    if (!ok)
    {
        // exit it
        if (process) tb_process_exit((tb_process_ref_t)process);
        process = tb_null;
    }

    // ok?
    return (tb_process_ref_t)process;
}
#endif
tb_process_ref_t tb_process_init_cmd(tb_char_t const* cmd, tb_process_attr_ref_t attr)
{
    // check
    tb_assert_and_check_return_val(cmd, tb_null);

    // done
    tb_process_ref_t    process          = tb_null;
    tb_char_t*          buffer           = tb_null;
    tb_char_t const**   argv             = tb_null;
    tb_char_t const*    argv_buffer[256] = {tb_null};
    do
    {
        // make buffer
        tb_size_t maxn = TB_PATH_MAXN;
        buffer = (tb_char_t*)tb_malloc(maxn);
        tb_assert_and_check_break(buffer);

        // copy and translate command
        tb_char_t   ch;
        tb_size_t   i = 0;
        tb_size_t   j = 0;
        tb_size_t   argv_maxn = 16;
        for (i = 0; j <= maxn && (ch = cmd[i]); i++)
        {
            // not enough? grow it
            if (j == maxn)
            {
                // grow it
                maxn    += TB_PATH_MAXN;
                buffer  = (tb_char_t*)tb_ralloc(buffer, maxn);
                tb_assert_and_check_break(buffer);
            }

            // translate "\"", "\'", "\\"
            tb_char_t next = cmd[i + 1];
            if (ch == '\\' && (next == '\"' || next == '\'' || next == '\\')) /* skip it */ ;
            // copy it
            else buffer[j++] = ch;

            // guess the argv max count
            if (tb_isspace(ch)) argv_maxn++;
        }
        tb_assert_and_check_break(j < maxn);
        buffer[j] = '\0';

        // ensure the argv buffer
        if (argv_maxn <= tb_arrayn(argv_buffer))
        {
            argv        = argv_buffer;
            argv_maxn   = tb_arrayn(argv_buffer);
        }
        else
        {
            // too large?
            if (argv_maxn > TB_MAXU16) argv_maxn = TB_MAXU16;

            // malloc the argv buffer
            argv = tb_nalloc0_type(argv_maxn, tb_char_t const*);
            tb_assert_and_check_break(argv);
        }

        // reset index
        i = 0;

        // parse command to the arguments
        tb_bool_t   s = 0;
        tb_size_t   m = argv_maxn;
        tb_char_t*  p = buffer;
        tb_char_t*  b = tb_null;
        while ((ch = *p))
        {
            // enter double quote?
            if (!s && ch == '\"') s = 2;
            // enter single quote?
            else if (!s && ch == '\'') s = 1;
            // leave quote?
            else if ((s == 2 && ch == '\"') || (s == 1 && ch == '\'')) s = 0;
            // is argument end with ' '?
            else if (!s && tb_isspace(ch))
            {
                // fill zero
                *p = '\0';

                // save this argument 
                if (b)
                {
                    // trace
                    tb_trace_d("argv: %s", b);

                    // save it
                    if (i < m - 1) argv[i++] = b;

                    // clear it
                    b = tb_null;
                }
            }

            // get the argument pointer
            if ((s || !tb_isspace(ch)) && !b) b = p;

            // next 
            p++;
        }
        
        // save this argument 
        if (b)
        {
            // trace
            tb_trace_d("argv: %s", b);

            // save it
            if (i < m - 1) argv[i++] = b;

            // clear it
            b = tb_null;
        }

        // check
        tb_assertf_and_check_break(i < m - 1, "the command(%s) arguments are too much!", cmd);

        // init process
        process = tb_process_init(argv[0], argv, attr);
    
    } while (0);

    // exit buffer
    if (buffer) tb_free(buffer);
    buffer = tb_null;

    // exit argv buffer
    if (argv != argv_buffer) tb_free(argv);
    argv = tb_null;

    // ok?
    return process;
}
tb_void_t tb_process_exit(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return(process);

#ifdef TB_CONFIG_MODULE_HAVE_COROUTINE
    // attempt to cancel waiting from coroutine first
    tb_pointer_t scheduler_io = tb_null;
    tb_poller_object_t object;
    object.type     = TB_POLLER_OBJECT_PROC;
    object.ref.proc = self;
#   ifndef TB_CONFIG_MICRO_ENABLE
    if ((scheduler_io = tb_co_scheduler_io_self()) && tb_co_scheduler_io_cancel((tb_co_scheduler_io_ref_t)scheduler_io, &object)) {}
    else
#   endif
    if ((scheduler_io = tb_lo_scheduler_io_self()) && tb_lo_scheduler_io_cancel((tb_lo_scheduler_io_ref_t)scheduler_io, &object)) {}
#endif

#ifdef TB_CONFIG_POSIX_HAVE_POSIX_SPAWNP
    // exit spawn attributes
    posix_spawnattr_destroy(&process->spawn_attr);

    // exit spawn action 
    posix_spawn_file_actions_destroy(&process->spawn_action);
#endif

    // exit it
    tb_free(process);
}
tb_void_t tb_process_kill(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return(process);

    // kill it
    if (process->pid > 0)
    {
#ifdef TB_CONFIG_LIBC_HAVE_KILL
        kill(process->pid, SIGKILL);
#else
        // noimpl
        tb_trace_noimpl();
#endif
    }
}
tb_cpointer_t tb_process_priv(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return_val(process, tb_null);

    return process->priv;
}
tb_void_t tb_process_priv_set(tb_process_ref_t self, tb_cpointer_t priv)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return(process);

    process->priv = priv;
}
tb_void_t tb_process_resume(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return(process);

    // resume it
    if (process->pid > 0)
    {
#ifdef TB_CONFIG_LIBC_HAVE_KILL
        kill(process->pid, SIGCONT);
#else
        // noimpl
        tb_trace_noimpl();
#endif
    }
}
tb_void_t tb_process_suspend(tb_process_ref_t self)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return(process);

    // suspend it
    if (process->pid > 0)
    {
#ifdef TB_CONFIG_LIBC_HAVE_KILL
        kill(process->pid, SIGSTOP);
#else
        // noimpl
        tb_trace_noimpl();
#endif
    }
}
tb_long_t tb_process_wait(tb_process_ref_t self, tb_long_t* pstatus, tb_long_t timeout)
{
    // check
    tb_process_t* process = (tb_process_t*)self;
    tb_assert_and_check_return_val(process, -1);

#if defined(TB_CONFIG_MODULE_HAVE_COROUTINE) \
        && !defined(TB_CONFIG_MICRO_ENABLE)
    // attempt to wait it in coroutine if timeout is non-zero
    if (timeout && tb_coroutine_self()) 
    {
        tb_poller_object_t object;
        object.type = TB_POLLER_OBJECT_PROC;
        object.ref.proc = self;
        return tb_coroutine_waitproc(&object, pstatus, timeout);
    }
#endif

    // done
    tb_long_t ok = 0;
    tb_hong_t time = tb_mclock();
    do
    {
        // wait it
        tb_int_t    status = -1;
        tb_long_t   result = waitpid(process->pid, &status, timeout < 0? 0 : WNOHANG | WUNTRACED);
        tb_check_return_val(result != -1, -1);

        // exited?
        if (result != 0)
        {
            /* save status, only get 8bits retval
             *
             * tt's limited to 8-bits, which means 1 byte, 
             * which means the int from WEXITSTATUS can only range from 0-255. 
             *
             * in fact, any unix program will only ever return a max of 255.
             */
            if (pstatus) *pstatus = WIFEXITED(status)? WEXITSTATUS(status) : -1;

            // clear pid
            process->pid = 0;

            // wait ok
            ok = 1;

            // end
            break;
        }

        // wait some time
        if (timeout > 0) tb_msleep(tb_min(timeout, 60));

    } while (timeout > 0 && tb_mclock() - time < (tb_hong_t)timeout);

    // ok?
    return ok;
}
tb_long_t tb_process_waitlist(tb_process_ref_t const* processes, tb_process_waitinfo_ref_t infolist, tb_size_t infomaxn, tb_long_t timeout)
{
    // check
    tb_assert_and_check_return_val(processes && infolist && infomaxn, -1);

    // done
    tb_long_t infosize = 0;
    tb_hong_t time = tb_mclock();
    do
    {
        // wait it
        tb_int_t    status = -1;
        tb_long_t   result = waitpid(-1, &status, timeout < 0? 0 : WNOHANG | WUNTRACED);
        tb_check_return_val(result != -1, -1);

        // exited?
        if (result != 0)
        {
            // find this process 
            tb_process_t const** pprocess = (tb_process_t const**)processes;
            for (; *pprocess && (*pprocess)->pid != result; pprocess++) ;

            // found?
            if (*pprocess)
            {
                // save process info
                infolist[infosize].index = (tb_process_ref_t const*)pprocess - processes;
                infolist[infosize].process = (tb_process_ref_t)*pprocess;
                infolist[infosize].status = WIFEXITED(status)? WEXITSTATUS(status) : -1;
                infosize++;

                // attempt to wait other processes
                while (infosize < infomaxn)
                {
                    // attempt to wait it
                    status = -1;
                    result = waitpid(-1, &status, WNOHANG | WUNTRACED);

                    // error or timeout? end
                    tb_check_break(result != 0);

                    // find this process 
                    tb_process_t const** pprocess = (tb_process_t const**)processes;
                    for (; *pprocess && (*pprocess)->pid != result; pprocess++) ;

                    // found?
                    if (*pprocess)
                    {
                        // save process info
                        infolist[infosize].index = (tb_process_ref_t const*)pprocess - processes;
                        infolist[infosize].process = (tb_process_ref_t)*pprocess;
                        infolist[infosize].status = WIFEXITED(status)? WEXITSTATUS(status) : -1;
                        infosize++;
                    }
                    else break;
                }

                // end
                break;
            }
        }

        /* wait some time, TODO we need use ISGCHLD and select to improve it
         * @see https://stackoverflow.com/questions/282176/waitpid-equivalent-with-timeout
         */
        if (timeout > 0) tb_msleep(tb_min(timeout, 60));

    } while (timeout > 0 && tb_mclock() - time < (tb_hong_t)timeout);

    // ok?
    return infosize;
}
