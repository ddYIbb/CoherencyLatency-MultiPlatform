import hashlib, os
root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")
names = ["CoherencyLatency-arm64-v8a", "CoherencyLatency-armeabi-v7a", "CoherencyLatency-x86", "CoherencyLatency-x86_64"]
lines = []
for n in names:
    p = os.path.join(root, n)
    h = hashlib.sha256(open(p, "rb").read()).hexdigest()
    lines.append("%s  %s" % (h, n))
    print("%-32s %10d  %s" % (n, os.path.getsize(p), h))
open(os.path.join(root, "SHA256SUMS"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
