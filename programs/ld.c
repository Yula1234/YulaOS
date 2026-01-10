// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {

    const char* external_libs[] = {
        "/bin/start.o", "/bin/malloc.o",
        "/bin/string.o", "/bin/stdlib.o",
        "/bin/stdio.o"
    };

    int libs_count = sizeof(external_libs) / sizeof(external_libs[0]);

    char** final_args = malloc((argc + libs_count + 1) * sizeof(char*));

    if (final_args == NULL) {
        printf("OOM");
        return 1;
    }

    int i;
    for (i = 0; i < argc; i++) {
        final_args[i] = argv[i];
    }

    for (int j = 0; j < libs_count; j++) {
        final_args[i + j] = (char*)external_libs[j];
    }

    final_args[argc + libs_count] = NULL;
	final_args[0] = "uld";

	int pid = spawn_process("/bin/uld.exe", argc + libs_count, final_args);

	if(pid < 0) {
		printf("ERROR: unable to find /bin/uld.exe");
		free(final_args);
		return -1;
	}

	int status = -1;

	if(waitpid(pid, &status) != pid) {
		printf("something went wrong with uld.exe");
		free(final_args);
		return -1;
	}

	if(status != 0) printf("the linking was not successful, error code: %d\n", status);
    
    free(final_args);
    return status;
}