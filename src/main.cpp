// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>
#include <exception>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "utils/limits.h"

#include "protocols/vscodeprotocol.h"
#include "debugger/manageddebugger.h"
#include "protocols/miprotocol.h"
#include "protocols/cliprotocol.h"
#include "managed/interop.h"
#include "utils/utf.h"
#include "utils/logger.h"
#include "buildinfo.h"
#include "version.h"

#ifdef INTEROP_DEBUGGING
#include "debugger/sigaction.h"
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define PATH_MAX MAX_PATH
static void setenv(const char* var, const char* val, int) { _putenv_s(var, val); }
#define getpid() (GetCurrentProcessId())
#else
#define _isatty(fd) ::isatty(fd)
#define _fileno(file) ::fileno(file)
#endif

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/types.h>
#include <unistd.h>
#endif


namespace netcoredbg
{

static const uint16_t DEFAULT_SERVER_PORT = 4711;

static void print_help()
{
    fprintf(stdout,
        ".NET Core debugger\n"
        "\n"
        "Options:\n"
        "--buildinfo                           Print build info.\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=cli                     Runs the debugger with Command Line Interface. \n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n"
        "--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.\n"
#ifdef INTEROP_DEBUGGING
        "--interop-debugging                   Puts the debugger into interop (mixed) mode.\n"
#endif
        "--command=<file>                      Interpret commands file at the start.\n"
        "-ex \"<command>\"                       Execute command at the start\n"
#ifdef NCDB_DOTNET_STARTUP_HOOK
        "--hot-reload                          Enable Hot Reload feature.\n"
#endif
        "--run                                 Run program without waiting commands\n"
        "--no-redirect                         Don't redirect debuggee's stdin/stdout/stderr into\n"
        "                                      protocol's output events: debuggee inherits debugger's\n"
        "                                      standard files, so its console is the terminal the\n"
        "                                      debugger was started in.\n"
        "--engineLogging[=<path to log file>]  Enable logging to VsDbg-UI or file for the engine.\n"
        "                                      Only supported by the VsCode interpreter.\n"
        "--server[=port_num]                   Start the debugger listening for requests on the\n"
        "                                      specified TCP/IP port instead of stdin/out. If port is not specified\n"
        "                                      TCP %i will be used. If port is 0, an arbitrary free port\n"
        "                                      is used (see --server-port-file).\n"
        "--server-port-file=<file>             Write the TCP/IP port the debugger listens on to <file>.\n"
        "                                      Allows the client which started the debugger with `--server=0`\n"
        "                                      to find out which port was really used.\n"
        "--log[=<type>]                        Enable logging. Supported logging to file and to dlog (only for Tizen)\n"
        "                                      File log by default. File is created in 'current' folder.\n"
        "--version                             Displays the current version.\n",
        (int)DEFAULT_SERVER_PORT
    );
}

static void print_buildinfo()
{
    printf(".NET Core debugger %s (%s)\n", __VERSION, BuildInfo::version);

    printf(
        "\nBuild info:\n"
        "      Build type:  %s\n"
        "      Build date:  %s %s\n"
        "      Target OS:   %s\n"
        "      Target arch: %s\n\n",
            BuildInfo::build_type,
            BuildInfo::date, BuildInfo::time,
            BuildInfo::os_name,
            BuildInfo::cpu_arch
    );

    printf("NetcoreDBG VCS info:  %s\n", BuildInfo::netcoredbg_vcs_info);
    printf("CoreCLR VCS info:     %s\n", BuildInfo::coreclr_vcs_info);
}

// protocol names for logging
template <typename ProtocolType> struct ProtocolDetails { static const char name[]; };
template <> const char ProtocolDetails<MIProtocol>::name[] = "MIProtocol";
template <> const char ProtocolDetails<VSCodeProtocol>::name[] = "VSCodeProtocol";
template <> const char ProtocolDetails<CLIProtocol>::name[] = "CLIProtocol";

// argument needed for protocol creation
using Streams = std::pair<std::istream&, std::ostream&>;

using ProtocolHolder = std::shared_ptr<IProtocol>;
using ProtocolConstructor = ProtocolHolder (*)(Streams);

// static functions which used to create protocol instance (like class fabric)
template <typename ProtocolType>
ProtocolHolder instantiate_protocol(Streams streams)
{
    LOGI("Creating protocol %s", ProtocolDetails<ProtocolType>::name);
    return ProtocolHolder{new ProtocolType(streams.first, streams.second)};
}

template <>
ProtocolHolder instantiate_protocol<CLIProtocol>(Streams streams)
{
    using ProtocolType = CLIProtocol;
    LOGI("Creating protocol %s", ProtocolDetails<ProtocolType>::name);
    return ProtocolHolder{new ProtocolType(dynamic_cast<InStream&>(streams.first), dynamic_cast<OutStream&>(streams.second))};
}


// Function writes TCP/IP port, debugger listens on, to the file `path`. File is created
// atomically (via rename), so that client never reads partially written file.
static void write_server_port_file(const std::string &path, unsigned port)
{
    std::string tmpPath = path + ".tmp";

    FILE *file = fopen(tmpPath.c_str(), "w");
    if (!file)
    {
        fprintf(stderr, "can't create %s: %s\n", tmpPath.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }

    fprintf(file, "%u\n", port);
    if (fclose(file) != 0 || rename(tmpPath.c_str(), path.c_str()) != 0)
    {
        fprintf(stderr, "can't write %s: %s\n", path.c_str(), strerror(errno));
        remove(tmpPath.c_str());
        exit(EXIT_FAILURE);
    }
}

// function creates pair of input/output streams for debugger protocol
template <typename Holder>
Streams open_streams(Holder& holder, bool server_mode, unsigned server_port,
                     const std::string &server_port_file, ProtocolConstructor constructor)
{
    if (server_mode)
    {
        IOSystem::FileHandle listening = IOSystem::listen_socket(server_port);
        if (! listening)
        {
            fprintf(stderr, "can't open listening socket for port %u\n", server_port);
            exit(EXIT_FAILURE);
        }

        // Note, `server_port` now holds the port really used (`--server=0` asks OS to pick one),
        // report it before `accept_socket()` blocks waiting for the client to connect.
        if (!server_port_file.empty())
            write_server_port_file(server_port_file, server_port);

        IOSystem::FileHandle socket = IOSystem::accept_socket(listening);
        if (! socket)
        {
            fprintf(stderr, "can't accept connection on port %u\n", server_port);
            exit(EXIT_FAILURE);
        }

        std::iostream *stream = new IOStream(StreamBuf(socket));
        holder.push_back(typename Holder::value_type{stream});
        return {*stream, *stream};
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (constructor == instantiate_protocol<CLIProtocol>)
    {
        IOSystem::StdFiles stdio = IOSystem::get_std_files();
        auto cin = new InStream(InStreamBuf(std::get<IOSystem::Stdin>(stdio)));
        auto cout = new OutStream(OutStreamBuf(std::get<IOSystem::Stdout>(stdio)));
        holder.push_back(typename Holder::value_type{cin}),
        holder.push_back(typename Holder::value_type{cout});
        return {*cin, *cout};
    }

    return {std::cin, std::cout};
}

} // namespace netcoredbg


using namespace netcoredbg;

static void FindAndParseArgs(char **argv, std::vector<std::pair<std::string, std::function<void(int& i)>>> &partialArguments, int i)
{
    for(auto argument:partialArguments)
    {
        if (strstr(argv[i], argument.first.c_str()) == argv[i])
        {
            argument.second(i);
            return;
        }
    }
    fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
    exit(EXIT_FAILURE);
}

static void CheckStartOptions(ProtocolConstructor &protocol_constructor, std::vector<string_view> &initCommands,
                              char* argv[], std::string &execFile, bool run, bool serverMode,
                              const std::string &serverPortFile)
{
    if (protocol_constructor != &instantiate_protocol<CLIProtocol> && !initCommands.empty())
    {
        fprintf(stderr, "%s: options -ex and --command can be used only with CLI interpreter!\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (run && execFile.empty())
    {
        fprintf(stderr, "--run option was given, but no executable file specified!\n");
        exit(EXIT_FAILURE);
    }

    if (protocol_constructor == &instantiate_protocol<CLIProtocol> && serverMode)
    {
        fprintf(stderr, "server mode can't be used with CLI interpreter!\n");
        exit(EXIT_FAILURE);
    }

    if (!serverPortFile.empty() && !serverMode)
    {
        fprintf(stderr, "--server-port-file option can be used only with --server!\n");
        exit(EXIT_FAILURE);
    }
}

static HRESULT AttachToExistingProcess(IDebugger *pDebugger, DWORD pidDebuggee)
{
    HRESULT Status;
    IfFailRet(pDebugger->Initialize());
    IfFailRet(pDebugger->Attach(pidDebuggee));
    return pDebugger->ConfigurationDone();
}

static HRESULT LaunchNewProcess(IDebugger *pDebugger, std::string &execFile, std::vector<std::string> &execArgs)
{
    HRESULT Status;
    IfFailRet(pDebugger->Initialize());

    try
    {
        IfFailRet(pDebugger->Launch(execFile, execArgs, {}, {}, false));
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "%s\n", e.what());
        exit(EXIT_FAILURE);
    }

    return pDebugger->ConfigurationDone();
}

int
#if defined(WIN32) && defined(_TARGET_X86_)
    __cdecl
#endif
            main(int argc, char* argv[])
{

    DWORD pidDebuggee = 0;
    // prevent std::cout flush triggered by read operation on std::cin
    std::cin.tie(nullptr);

    ProtocolConstructor protocol_constructor =
#ifdef DEBUGGER_FOR_TIZEN
        &instantiate_protocol<MIProtocol>;
#else
        _isatty(_fileno(stdin)) ?  &instantiate_protocol<CLIProtocol> : &instantiate_protocol<MIProtocol>;
#endif

    bool engineLogging = false;
    std::string logFilePath;

    std::vector<std::string> initTexts;
    std::vector<string_view> initCommands;

    bool serverMode = false;
    unsigned serverPort = 0;
    std::string serverPortFile;

    std::string execFile;
    std::vector<std::string> execArgs;

    bool needHotReload = false;
    bool needInteropDebugging = false;
    bool noRedirect = false;
    bool run = false;

    std::unordered_map<std::string, std::function<void(int& i)>> entireArguments
    {
        {"--attach", [&](int& i){

            i++;
            if (i >= argc)
            {
                fprintf(stderr, "Error: Missing process id\n");
                exit(EXIT_FAILURE);
            }
            char *err;
            pidDebuggee = strtoul(argv[i], &err, 10);
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                exit(EXIT_FAILURE);
            }

        } },
        { "--interpreter=mi", [&](int& i){

            protocol_constructor = &instantiate_protocol<MIProtocol>;

        } },
        { "--interpreter=vscode", [&](int& i){

            protocol_constructor = &instantiate_protocol<VSCodeProtocol>;

        } },
        { "--interpreter=cli", [&](int& i){

            protocol_constructor = &instantiate_protocol<CLIProtocol>;

        } },
        { "--interop-debugging", [&](int& i){

            needInteropDebugging = true;

        } },
        { "--hot-reload", [&](int& i){

            needHotReload = true;

        } },
        { "--no-redirect", [&](int& i){

            noRedirect = true;

        } },
        { "--run", [&](int& i){

            run = true;

        } },
        { "-ex", [&](int& i){

            if (++i >= argc)
            {
                fprintf(stderr, "%s: -ex option requires an argument!\n", argv[0]);
                exit(EXIT_FAILURE);
            }
            initCommands.emplace_back(argv[i]);

        } },
        { "--engineLogging", [&](int& i){

            engineLogging = true;

        } },
        { "--help", [&](int& i){

            print_help();
            exit(EXIT_SUCCESS);

        } },
        { "--buildinfo", [&](int& i){

            print_buildinfo();
            exit(EXIT_SUCCESS);

        } },
        { "--version", [&](int& i){

            fprintf(stdout, "NET Core debugger %s (%s, %s)\n",
                __VERSION, BuildInfo::netcoredbg_vcs_info, BuildInfo::build_type);
            fprintf(stdout, "\nCopyright (c) 2020 Samsung Electronics Co., LTD\n");
            fprintf(stdout, "Distributed under the MIT License.\n");
            fprintf(stdout, "See the LICENSE file in the project root for more information.\n");
            exit(EXIT_SUCCESS);

        } },
        { "--log", [&](int& i){

            #ifdef _WIN32
            static const char path_separator[] = "/\\";
            #else
            static const char path_separator[] = "/";
            #endif

            // somethat similar to basename(3)
            char *s = argv[0] + strlen(argv[0]);
            while (s > argv[0] && !strchr(path_separator, s[-1])) s--;

            char tmp[PATH_MAX];
            auto tempdir = GetTempDir();
            snprintf(tmp, sizeof(tmp), "%.*s/%s.%u.log", int(tempdir.size()), tempdir.data(), s, getpid());
            setenv("LOG_OUTPUT", tmp, 1);

        } },
        { "--server", [&](int& i){

            serverMode = true;
            serverPort = DEFAULT_SERVER_PORT;

        } },
        { "--", [&](int& i){

            ++i;
            if (i < argc)
            {
                execFile = argv[i];
            }
            else
            {
                fprintf(stderr, "Error: Missing program argument\n");
                exit(EXIT_FAILURE);
            }
            for (++i; i < argc; ++i)
            {
                execArgs.push_back(argv[i]);
            }
        } }
    };

    std::vector<std::pair<std::string, std::function<void(int& i)>>> partialArguments
    {
        { "--command=", [&](int& i){

            initTexts.push_back(std::string() + "source " + (strchr(argv[i], '=') + 1));
            initCommands.push_back(initTexts.back());

        } },
        { "--engineLogging=", [&](int& i){

            engineLogging = true;
            logFilePath = argv[i] + strlen("--engineLogging=");

        } },
        { "--log=", [&](int& i){

            setenv("LOG_OUTPUT", argv[i] + strlen("--log="), 1);

        } },
        { "--server=", [&](int& i){

            char *err;
            const char *arg = argv[i] + strlen("--server=");
            unsigned long port = strtoul(arg, &err, 10);
            if (*arg == 0 || *err != 0 || port > 65535)
            {
                fprintf(stderr, "Error: Invalid server port\n");
                exit(EXIT_FAILURE);
            }
            serverMode = true;
            serverPort = static_cast<unsigned>(port);

        } },
        { "--server-port-file=", [&](int& i){

            serverPortFile = argv[i] + strlen("--server-port-file=");
            if (serverPortFile.empty())
            {
                fprintf(stderr, "Error: Missing server port file name\n");
                exit(EXIT_FAILURE);
            }

        } },
    };

    for (int i = 1; i < argc; i++)
    {
        auto args = entireArguments.find(std::string(argv[i]));
        if (args != entireArguments.end())
        {
            args->second(i);
        }
        else
        {
            FindAndParseArgs(argv, partialArguments, i);
        }
    }

    CheckStartOptions(protocol_constructor, initCommands, argv, execFile, run, serverMode, serverPortFile);

    LOGI("Netcoredbg started");
    // Note: there is no possibility to know which exception caused call to std::terminate
    std::set_terminate([]{ LOGF("Netcoredbg is terminated due to call to std::terminate: see stderr..."); });

    std::vector<std::unique_ptr<std::ios_base> > streams;
    std::shared_ptr<IProtocol> protocol = protocol_constructor(open_streams(streams, serverMode, serverPort, serverPortFile, protocol_constructor));

    if (engineLogging)
    {
        auto p = dynamic_cast<VSCodeProtocol*>(protocol.get());
        if (!p)
        {
            fprintf(stderr, "Error: Engine logging is only supported in VsCode interpreter mode.\n");
            LOGE("Engine logging is only supported in VsCode interpreter mode.");
            exit(EXIT_FAILURE);
        }

        p->EngineLogging(logFilePath);
    }

    std::shared_ptr<IDebugger> debugger;
    try
    {
        debugger.reset(new ManagedDebugger(protocol.get()));
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "%s\n", e.what());
        exit(EXIT_FAILURE);
    }

    protocol->SetDebugger(debugger);
    debugger->SetRedirectStdio(!noRedirect);
    if (needHotReload)
    {
        if (pidDebuggee == 0)
            debugger->SetHotReload(needHotReload);
        else
            fprintf(stderr, "Warning: Hot Reload can't be be enabled for attached process.\n");
    }
#ifdef INTEROP_DEBUGGING
    // In case of interop debugging we depend on SIGCHLD set to SIG_DFL by init code.
    // Note, debugger include corhost (CoreCLR) that could setup sigaction for SIGCHLD and ruin interop debugger work.
    SetSigactionMode(needInteropDebugging);
    debugger->SetInteropDebugging(needInteropDebugging);
#endif

    if (!execFile.empty())
        protocol->SetLaunchCommand(execFile, execArgs);

    LOGI("pidDebugee %d", pidDebuggee);
    HRESULT Status;
    if (pidDebuggee != 0 && FAILED(Status = AttachToExistingProcess(debugger.get(), pidDebuggee)))
    {
        fprintf(stderr, "Error: 0x%x Failed to attach to %i\n", Status, pidDebuggee);
        Interop::Shutdown();
        return EXIT_FAILURE;
    }
    else if (run && FAILED(Status = LaunchNewProcess(debugger.get(), execFile, execArgs)))
    {
        fprintf(stderr, "Error: %#x %s\n", Status, errormessage(Status));
        Interop::Shutdown();
        return EXIT_FAILURE;
    }

    // switch CLIProtocol to asynchronous mode when attaching
    auto cliProtocol = dynamic_cast<CLIProtocol*>(protocol.get());
    if (cliProtocol)
    {
        if (pidDebuggee != 0)
            cliProtocol->SetCommandMode(CLIProtocol::CommandMode::Asynchronous);

        // inform CLIProtocol that process is already running
        if (run || pidDebuggee)
            cliProtocol->SetRunningState();

        if (pidDebuggee != 0)
            cliProtocol->Pause();

        // run commands passed in command line via '-ex' option
        cliProtocol->Source({initCommands});
    }

    protocol->CommandLoop();
    Interop::Shutdown();
    return EXIT_SUCCESS;
}