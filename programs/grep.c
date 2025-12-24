#include <yula.h>

#define LINE_MAX 2048
#define READ_CHUNK 4096

#define C_BG       0x141414
#define C_TEXT     0xD4D4D4
#define C_MATCH    0xF44747
#define C_FILE     0xC586C0
#define C_COORDS   0xB5CEA8
#define C_SEP      0x606060

const char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return haystack;
        }
    }
    return 0;
}

void process_line(const char* line, const char* pattern, const char* filename, int line_num) {
    const char* first_match = strstr(line, pattern);
    
    if (!first_match) return;

    int col = (int)(first_match - line) + 1;

    if (filename) {
        set_console_color(C_FILE, C_BG);
        print(filename);
        set_console_color(C_SEP, C_BG);
        print(":");
    }

    set_console_color(C_COORDS, C_BG);
    print_dec(line_num);
    set_console_color(C_SEP, C_BG);
    print(":");
    
    set_console_color(C_COORDS, C_BG);
    print_dec(col);
    set_console_color(C_SEP, C_BG);
    print(": ");
    
    const char* ptr = line;
    const char* match;
    int pat_len = strlen(pattern);

    while ((match = strstr(ptr, pattern))) {
        while (ptr < match) {
            char tmp[2] = {*ptr++, 0};
            set_console_color(C_TEXT, C_BG);
            print(tmp);
        }

        set_console_color(C_MATCH, C_BG);
        print(pattern);
        
        ptr += pat_len;
    }

    set_console_color(C_TEXT, C_BG);
    print(ptr);
    print("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: grep <pattern> [file]\n");
        return 1;
    }

    const char* pattern = argv[1];
    const char* filename = 0;
    int fd = 0;

    if (argc > 2) {
        filename = argv[2];
        fd = open(filename, 0);
        if (fd < 0) {
            set_console_color(C_MATCH, C_BG);
            printf("grep: %s: No such file\n", filename);
            set_console_color(C_TEXT, C_BG);
            return 1;
        }
    }

    char chunk[READ_CHUNK];
    char line[LINE_MAX];
    int line_pos = 0;
    int line_num = 1;
    int n;

    while ((n = read(fd, chunk, READ_CHUNK)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            
            if (c == '\n') {
                line[line_pos] = 0;
                process_line(line, pattern, filename, line_num);
                
                line_pos = 0;
                line_num++;
            } else {
                if (line_pos < LINE_MAX - 1) {
                    if (c != '\r') line[line_pos++] = c;
                }
            }
        }
    }

    if (line_pos > 0) {
        line[line_pos] = 0;
        process_line(line, pattern, filename, line_num);
    }

    if (fd != 0) close(fd);
    
    set_console_color(C_TEXT, C_BG);
    return 0;
}