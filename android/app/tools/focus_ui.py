import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
def walk(n, depth=0):
    a = n.attrib
    if a.get('focused') == 'true' or a.get('selected') == 'true' or (a.get('clickable') == 'true' and a.get('enabled') == 'true'):
        print(('  ' * depth) + f"{a.get('class','').split('.')[-1]:18} text={a.get('text','')!r:32} bounds={a.get('bounds','')} focusable={a.get('focusable')} focused={a.get('focused')} clickable={a.get('clickable')} enabled={a.get('enabled')}")
    for c in n: walk(c, depth+1)
walk(root)
