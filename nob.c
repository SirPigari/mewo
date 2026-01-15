#define NOB_IMPLEMENTATION
#include "./thirdparty/nob.h"
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    bool release = argc >= 2 && strcmp(argv[1], "release") == 0;

    Nob_Cmd cmd = {0};

#ifdef _WIN32
    if (release) {
        const char *vs_path = "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools";
        const char *bat_path = "build_release.bat";

        FILE *f = fopen(bat_path, "w");
        if (!f) {
            fprintf(stderr, "failed to create %s\n", bat_path);
            return 1;
        }

        fprintf(f,
            "@echo off\n"
            "call \"%s\\Common7\\Tools\\VsDevCmd.bat\" -arch=x64\n"
            "clang-cl source\\main.c /Os /MT /D_CRT_SECURE_NO_WARNINGS /Oi /Ot /Gy /link /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE /OUT:mewo.exe\n",
            vs_path
        );

        fclose(f);

        Nob_Cmd ps = {0};
        nob_cmd_append(&ps, "cmd.exe", "/C", bat_path);

        if (!nob_cmd_run(&ps)) return 1;

        remove(bat_path);
    } else {
        nob_cmd_append(&cmd,
            "gcc",
            "source/main.c",
            "-o", "mewo.exe",
            "-g"
        );

        if (!nob_cmd_run(&cmd)) return 1;
    }
#else
    if (release) {
        nob_cmd_append(&cmd,
            "musl-gcc",
            "source/main.c",
            "-o", "mewo",
            "-Os",
            "-static",
            "-flto",
            "-fdata-sections",
            "-ffunction-sections",
            "-Wl,--gc-sections",
            "-s"
        );
    } else {
        nob_cmd_append(&cmd,
            "gcc",
            "source/main.c",
            "-o", "mewo",
            "-g"
        );
    }
    
    if (!nob_cmd_run(&cmd)) return 1;
#endif

    #ifdef _WIN32
    struct _stat64 st;
    if (_stat64("mewo.exe", &st) == 0)
        nob_log(NOB_INFO, "Built mewo.exe (%zu Kb)", (size_t)(st.st_size / 1024));
    #else
    struct stat st;
    if (stat("mewo", &st) == 0)
        nob_log(NOB_INFO, "Built mewo (%zu Kb)", (size_t)(st.st_size / 1024));
    #endif

    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        Nob_Cmd run = {0};
#ifdef _WIN32
        nob_cmd_append(&run, "./mewo.exe");
#else
        nob_cmd_append(&run, "./mewo");
#endif
        for (int i = 2; i < argc; ++i) {
            nob_cmd_append(&run, argv[i]);
        }
        if (!nob_cmd_run(&run)) return 1;
    }

    return 0;
}
