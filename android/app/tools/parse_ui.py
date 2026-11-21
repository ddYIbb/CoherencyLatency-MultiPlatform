import sys, xml.etree.ElementTree as ET
path = sys.argv[1]
root = ET.parse(path).getroot()
for node in root.iter("node"):
    text = node.get("text") or ""
    rid = node.get("resource-id") or ""
    cls = node.get("class") or ""
    if text or rid:
        print(repr(text), rid, cls, node.get("bounds"))
