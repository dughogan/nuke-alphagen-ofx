# Builds the DUGco AlphaGenSAM example script, example/dugco_alphagensam_example.nk, in a
# running Nuke (paste into the Script Editor, or exec this file). The Read loads
# example/diner_plate.mp4 relative to the script, so the example opens on any machine.
#
# One node = one matte: three AlphaGenSAM nodes branch off the same plate (car, person,
# diner). Car and Diner are typed; Person is a Ctrl+click on the walking man (typing
# "person" would also select the customers inside the diner). Their alphas are copied into
# named layers (car.a, person.a, diner.a) on one stream, with a cut-out preview of each.
import os

import nuke

# outside a file (e.g. pasted into Nuke's Script Editor) run it with the repo root as the working dir
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) if "__file__" in globals() else os.getcwd()
OUT = ROOT.replace("\\", "/") + "/example/dugco_alphagensam_example.nk"
PLATE = "[file dirname [value root.name]]/diner_plate.mp4"
FIRST, LAST = 1, 121
QUICK = (1, 25)  # each node processes this range first; set "frame range" to input range for the shot
# layer, title, tile colour, select, Ctrl+clicks ("x y +|- frame 1;..." in plate pixels, y up)
OBJECTS = [
    ("car", "Car", 0x3fb7c5ff, "car", ""),
    ("person", "Person", 0xe8913aff, "", "744.00 430.00 + 13 1;742.00 500.00 + 13 1"),
    ("diner", "Diner", 0xd1495bff, "diner", ""),
]

nuke.scriptClear()
root = nuke.root()
root["first_frame"].setValue(FIRST)
root["last_frame"].setValue(LAST)
root["fps"].setValue(24)
nuke.addFormat("1536 864 0 0 1536 864 1 DUGco_diner_1536")
root["format"].setValue("DUGco_diner_1536")
for layer, *_ in OBJECTS:
    nuke.Layer(layer, [layer + ".a"])


def place(n, x, y):
    n.setXYpos(int(x), int(y))
    return n


def backdrop(label, x, y, w, h, colour, size=42):
    b = nuke.nodes.BackdropNode(label=label, note_font_size=size, tile_color=colour, bdwidth=w, bdheight=h)
    b.setXYpos(x, y)
    return b


read = place(nuke.nodes.Read(name="Diner_Plate"), 0, 0)
read["file"].fromUserText(PLATE)
for k, v in (("first", FIRST), ("last", LAST), ("origfirst", FIRST), ("origlast", LAST)):
    read[k].setValue(v)
dot = place(nuke.nodes.Dot(inputs=[read]), 34, 140)

xs = [-420, 0, 420]
ag = []
for (layer, title, colour, sel, clicks), x in zip(OBJECTS, xs):
    n = nuke.createNode("OFXcom.dugco.AlphaGenSAM_v1", inpanel=False)
    n.setInput(0, dot)
    n.setName("AG_" + title)
    n["select"].setValue(sel)
    n["points"].setValue(clicks)
    n["rangeMode"].setValue("custom")
    n["frameStart"].setValue(QUICK[0])
    n["frameEnd"].setValue(QUICK[1])
    n["session"].setValue("")  # each user's copy gets its own on first Preview/Process
    n["tile_color"].setValue(colour)
    n["label"].setValue("matte: " + layer)
    place(n, x, 260)
    ag.append(n)

# all three alphas as named layers on one stream
stream = dot
for (layer, title, *_), n, i in zip(OBJECTS, ag, range(3)):
    stream = place(nuke.nodes.Copy(inputs=[stream, n], from0="rgba.alpha", to0=layer + ".a", name="Copy_" + title),
                   0, 420 + 90 * i)
out = place(nuke.nodes.Dot(inputs=[stream], label="plate + car.a / person.a / diner.a", note_font_size=20), 34, 720)

# cut-out previews: each layer back into the alpha, premultiplied over a dark checker
check = place(nuke.nodes.CheckerBoard2(boxsize=48, color0=[0.12, 0.12, 0.12, 1], color1=[0.2, 0.2, 0.2, 1],
                                        color2=[0.12, 0.12, 0.12, 1], color3=[0.2, 0.2, 0.2, 1], centerlinewidth=0,
                                        name="Checker"), 700, 820)
for (layer, title, *_), x in zip(OBJECTS, xs):
    s = place(nuke.nodes.Copy(inputs=[out, out], from0=layer + ".a", to0="rgba.alpha", name="Use_" + title), x, 840)
    p = place(nuke.nodes.Premult(inputs=[s], name="Premult_" + title), x, 900)
    place(nuke.nodes.Merge2(inputs=[check, p], operation="over", name="Cutout_" + title, label=title + " cut-out"),
          x, 980)

backdrop("DUGco AlphaGenSAM example\n<font size=4>One node = one matte. Car and Diner are typed; Person is a "
         "Ctrl+click on the walking man. Press Process on each node (frames 1-25; set frame range to input "
         "range for the whole shot).</font>", -520, 200, 1240, 160, 0x2a2f38ff)
backdrop("Alphas as layers", -100, 380, 330, 360, 0x323a2fff, 24)
backdrop("Cut-out previews", -520, 800, 1240, 260, 0x3a2f2fff, 24)

v = nuke.toNode("Viewer1") or nuke.nodes.Viewer()
v.setInput(0, ag[0])
place(v, -600, 260)
nuke.frame(13)
nuke.scriptSaveAs(OUT, overwrite=1)
print("saved", OUT)
