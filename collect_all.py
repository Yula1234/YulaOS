import os

SOURCE_DIRS = ['src', 'programs', 'usr', 'tools']
OUTPUT_FILE = 'all_files_content.txt'

def main():
    for SOURCE_DIR in SOURCE_DIRS:
        if not os.path.exists(SOURCE_DIR):
            print(f"Error: directory '{SOURCE_DIR}' not found.")
            return

    with open(OUTPUT_FILE, 'w', encoding='utf-8') as outfile:
        for SOURCE_DIR in SOURCE_DIRS:
            for root, dirs, files in os.walk(SOURCE_DIR):
                for file in files:
                    file_path = os.path.join(root, file)
                    
                    try:
                        with open(file_path, 'r', encoding='utf-8') as infile:
                            content = infile.read()
                            
                            outfile.write(f"File - {file_path}: ")
                            outfile.write(content)
                            outfile.write("\n")
                            
                            print(f"[+] Appended: {file_path}")
                            
                    except UnicodeDecodeError:
                        print(f"[-] Skiped (Binary/not UTF-8): {file_path}")
                    except Exception as e:
                        print(f"[!] Error while reading {file_path}: {e}")

    print(f"\nSuccess, output saved into {OUTPUT_FILE}")

if __name__ == "__main__":
    main()