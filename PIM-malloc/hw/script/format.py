import os


def format(dirpath: str):
    os.system(f"find {dirpath} -name \"*.h\" -exec clang-format -style=Google -i {{}} \\;")
    os.system(f"find {dirpath} -name \"*.cc\" -exec clang-format -style=Google -i {{}} \\;")
    os.system(f"find {dirpath} -name \"*.c\" -exec clang-format -style=Google -i {{}} \\;")


if __name__ == "__main__":
    script_dirpath = os.path.dirname(__file__)

    src_dirpath = os.path.join(script_dirpath, "..", "Sources")
    test_dirpath = os.path.join(script_dirpath, "..", "Tests")

    format(src_dirpath)
    format(test_dirpath)
    
