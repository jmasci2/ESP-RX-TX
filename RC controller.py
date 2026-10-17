# Author: AI Assistant
# Description: Generates an RC Controller shell with 2 Joysticks, a 4x4 Matrix Keypad cutout, and 6 rear button holes.

import adsk.core, adsk.fusion, adsk.cam, traceback

def run(context):
    ui = None
    try:
        app = adsk.core.Application.get()
        ui  = app.userInterface
        design = app.activeProduct
        
        # Get the root component of the active design
        rootComp = design.rootComponent
        
        # Create a new sketch on the x-y construction plane
        sketches = rootComp.sketches
        xyPlane = rootComp.xYConstructionPlane
        sketch = sketches.add(xyPlane)
        
        # 1. Main Controller Body Base Outline (220mm x 135mm -> 22.0cm x 13.5cm)
        w = 22.0  
        h = 13.5  
        lines = sketch.sketchCurves.sketchLines
        lines.addTwoPointRectangle(adsk.core.Point3D.create(-w/2, -h/2, 0), adsk.core.Point3D.create(w/2, h/2, 0))
        
        # 2. Keypad Cutout Window (41mm x 45mm -> 4.1cm x 4.5cm)
        kw = 4.1
        kh = 4.5
        lines.addTwoPointRectangle(adsk.core.Point3D.create(-kw/2, -kh/2, 0), adsk.core.Point3D.create(kw/2, kh/2, 0))
        
        # 3. Left Joystick Cutout (26mm x 26mm -> 2.6cm x 2.6cm centered at x = -7.5cm)
        jw = 2.6
        lines.addTwoPointRectangle(adsk.core.Point3D.create(-7.5 - jw/2, -jw/2, 0), adsk.core.Point3D.create(-7.5 + jw/2, jw/2, 0))
        
        # 4. Right Joystick Cutout (26mm x 26mm -> 2.6cm x 2.6cm centered at x = 7.5cm)
        lines.addTwoPointRectangle(adsk.core.Point3D.create(7.5 - jw/2, -jw/2, 0), adsk.core.Point3D.create(7.5 + jw/2, jw/2, 0))
        
        # 5. Rear Buttons Cutouts (6x 12mm holes -> 1.2cm diameter)
        # Left column (x = -8.5cm), Right column (x = 8.5cm), vertically stacked
        circles = sketch.sketchCurves.sketchCircles
        r_dia = 1.2
        
        y_positions = [-3.0, -4.5, -6.0]
        for y_pos in y_positions:
            # Left Buttons (B1, B2, B3)
            circles.addByCenterRadius(adsk.core.Point3D.create(-8.5, y_pos, 0), r_dia / 2)
            # Right Buttons (B4, B5, B6)
            circles.addByCenterRadius(adsk.core.Point3D.create(8.5, y_pos, 0), r_dia / 2)
            
        ui.messageBox('RC Controller Sketch Grid Generated Successfully!')
        
    except:
        if ui:
            ui.messageBox('Failed:\n{}'.format(traceback.format_exc()))
