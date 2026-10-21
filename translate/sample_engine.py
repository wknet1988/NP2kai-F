#!/usr/bin/env python3
# Sample translation engine for the NP2kai "internal text translation" feature.
#
# Protocol (the emulator calls it as):   engine <infile> <outfile>
#   * infile  : UTF-8 text, one original line per physical line
#   * outfile : UTF-8 text, one translation per physical line (same order/count)
#
# Replace translate_line() with a real translator: an online API, a local LLM
# (e.g. Ollama / llama.cpp), or an offline dictionary.  This template simply
# echoes the original so the whole capture -> translate -> display pipeline is
# demonstrable end-to-end even with no network.

import sys


def translate_line(text: str) -> str:
    # TODO: replace with a real translation call.
    #
    # Online example (needs network + an API key):
    #     import requests
    #     r = requests.post(API_URL, json={"q": text}, headers={...}, timeout=10)
    #     return r.json().get("result", text)
    #
    # Local LLM example:
    #     import subprocess
    #     return subprocess.run(["ollama", "run", "qwen2.5", text],
    #                           capture_output=True, text=True).stdout.strip()
    return text  # offline pass-through


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write("usage: sample_engine.py <infile> <outfile>\n")
        return 1
    infile, outfile = sys.argv[1], sys.argv[2]
    with open(infile, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = [translate_line(ln) for ln in lines]
    with open(outfile, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
