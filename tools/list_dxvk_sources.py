import re
from pathlib import Path

p = Path(r"C:\Users\amien\Documents\cursor\black-mesa-vr\third_party\l4d2vr\L4D2VR\l4d2vr.vcxproj")
text = p.read_text(encoding="utf-8")
files = re.findall(r'<ClCompile Include="([^"]+)"', text)
dxvk = [f.replace("..\\dxvk_new\\", "").replace("\\", "/") for f in files if "dxvk_new" in f]
print("count", len(dxvk))
Path(r"C:\Users\amien\Documents\cursor\black-mesa-vr\tools\dxvk_sources.txt").write_text("\n".join(dxvk) + "\n", encoding="utf-8")
for f in dxvk:
    print(f)
