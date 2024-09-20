import multiprocessing
from pathlib import Path

import math
import os
import re
from concurrent.futures import ProcessPoolExecutor

from praktika.utils import Utils, Shell
from praktika.result import Result


def find_files(include_dirs, exclude_dirs, file_suffixes):
    for dir_ in include_dirs:
        assert Path(dir_).is_dir()
        for root, dirs, files in os.walk(dir_):
            for file in files:
                if any([exclude_dir in root for exclude_dir in exclude_dirs]):
                    continue
                if not file_suffixes or any(
                    [file.endswith(suffix) for suffix in file_suffixes]
                ):
                    yield os.path.join(root, file)


def chunk_list(data, n):
    """Split the data list into n nearly equal-sized chunks."""
    chunk_size = math.ceil(len(data) / n)
    for i in range(0, len(data), chunk_size):
        yield data[i : i + chunk_size]


def run_check(include_dirs, exclude_dirs, file_extensions, check_function, check_name):
    stop_watch = Utils.Stopwatch()

    files = list(find_files(include_dirs, exclude_dirs, file_extensions))
    print(files)

    file_chunks = list(chunk_list(files, NPROC))

    results = []
    # Run check_function concurrently on each chunk
    with ProcessPoolExecutor(max_workers=NPROC) as executor:

        futures = [executor.submit(check_function, chunk) for chunk in file_chunks]

        # Wait for results and process them (optional)
        for future in futures:
            try:
                res = future.result()
                if res:
                    results.append(res)
            except Exception as e:
                results.append(f"Exception in {check_name}: {e}")

    result = Result(
        name=check_name,
        status=Result.Status.SUCCESS if not results else Result.Status.FAILED,
        start_time=stop_watch.start_time,
        duration=stop_watch.duration,
        info=f"errors: {results}" if results else "",
    )
    return result


def check_duplicate_includes(file_path):
    includes = []
    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if re.match(r"^#include ", line):
                includes.append(line.strip())

    include_counts = {line: includes.count(line) for line in includes}
    duplicates = {line: count for line, count in include_counts.items() if count > 1}

    if duplicates:
        return f"{file_path}: {duplicates}"
    return ""


def check_whitespaces(file_paths):
    if isinstance(file_paths, list):
        file_paths = " ".join(file_paths)
    exit_code, out, err = Shell.get_res_stdout_stderr(
        f"./utils/check-style/double-whitespaces.pl {file_paths}", verbose=False
    )
    return out


def check_yamllint(file_paths):
    if isinstance(file_paths, list):
        file_paths = " ".join(file_paths)
    exit_code, out, err = Shell.get_res_stdout_stderr(
        f"/usr/bin/yamllint --config-file=./.yamllint {file_paths}", verbose=False
    )
    return out or err


# def find_style_faults():
#     stop_watch = Utils.Stopwatch()
#     with ContextManager.cd("./utils/check-style"):
#         code, out, err = Shell.get_res_stdout_stderr("./check-style")
#         status = Result.Status.SUCCESS
#         info = ""
#         if code != 0 or out or err:
#             status = Result.Status.FAILED
#             info += f"exit code: {code}"
#             if out:
#                 info += f"\n  out: {out}"
#             if err:
#                 info += f"\n  err: {err}"
#
#         result = Result(
#             name="Check Style CPP",
#             status=status,
#             start_time=stop_watch.start_time,
#             duration=stop_watch.duration,
#             info=info
#         )
#     return result


if __name__ == "__main__":

    NPROC = multiprocessing.cpu_count()

    include_cpp_h_dirs = ["./src", "./base", "./programs", "./utils"]
    exclude_cpp_h_dirs = [
        "./base/glibc-compatibility",
        "./contrib/consistent-hashing",
        "./base/widechar_width",
    ]
    suffixes_cpp_h = [".h", ".cpp"]

    results = []
    stop_watch = Utils.Stopwatch()
    # # We decided to have the regexp-based check disabled in favor of clang-tidy
    # # results.append(find_all_duplicates())

    # results.append(
    #     run_check(
    #         include_dirs=include_cpp_h_dirs,
    #         exclude_dirs=exclude_cpp_h_dirs,
    #         file_extensions=suffixes_cpp_h,
    #         check_function=check_whitespaces,
    #         check_name="Whitespace Check",
    #     )
    # )

    results.append(
        run_check(
            include_dirs=["./.github"],
            exclude_dirs=[],
            file_extensions=[".yaml", ".yml"],
            check_function=check_yamllint,
            check_name="YamlLint Check",
        )
    )

    res = Result.generate_from(results=results, stopwatch=stop_watch).dump()
    print(res)
