"""Convert UTF-8 string literals in a C source to CP932 byte escapes.

The PC build gets this from GCC's -fexec-charset=CP932; VitaSDK's Windows GCC
has no iconv for that, so the Vita build rewrites the literals before compiling.
Comments and ASCII-only literals are left untouched.
"""
import sys

HEX = set(b"0123456789abcdefABCDEF")


def convert(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(src[i:j]); i = j; continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(src[i:j]); i = j; continue
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            body = src[i + 1:j]
            i = j + 1
            if all(ord(ch) < 0x80 for ch in body):
                out.append(q + body + q); continue
            if q == "'":
                raise SystemExit("non-ASCII char literal not supported")
            parts = [q]
            k = 0
            prev_hex = False
            while k < len(body):
                ch = body[k]
                if ch == "\\":
                    esc = body[k:k + 2]
                    if esc[1:2] == "x":
                        m = k + 2
                        while m < len(body) and body[m] in "0123456789abcdefABCDEF":
                            m += 1
                        esc = body[k:m]
                    parts.append(esc); k += len(esc)
                    prev_hex = esc.startswith("\\x")
                    continue
                if ord(ch) >= 0x80:
                    for b in ch.encode("cp932"):
                        parts.append("\\x%02x" % b)
                    prev_hex = True
                else:
                    if prev_hex and ch.encode()[0] in HEX:
                        parts.append(q + " " + q)
                    parts.append(ch)
                    prev_hex = False
                k += 1
            parts.append(q)
            out.append("".join(parts))
            continue
        out.append(c); i += 1
    return "".join(out)


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    with open(sys.argv[2], "w", encoding="utf-8", newline="") as f:
        f.write(convert(text))
