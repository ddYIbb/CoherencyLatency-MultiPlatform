import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
def walk(n, d=0):
    text=(n.get("text") or "").replace("\n"," | ")
    print("  "*d + f'{n.get("class")} bounds={n.get("bounds")} scroll={n.get("scrollable")} click={n.get("clickable")} enabled={n.get("enabled")} text={text[:80]!r}')
    for c in n: walk(c,d+1)
walk(root)
