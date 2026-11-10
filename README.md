# APEXeditor

![alt text](https://i.imgur.com/gSbcK7g.jpeg)




all local apex cfg / settings / Optimization

```
apex video cfg
Downloads and installs video config files (kerubimm), shows where they are placed, and launches the game.
Custom
Resolution, field of view, texture quality, and anti-aliasing. Each setting is written directly to the correct game config files.
Color
Saturation, brightness, contrast, and screen gamma. Detects running software to apply automatic color, plus per-application rules.
Optimization
Generates the optimization autoexec.cfg file. A "tuning" panel is planned for later (currently disabled).
Config
Saves and reloads your settings as named profiles. Startup options, startup logo, and file cleanup.
Spotify
Now playing, a detachable player that can be moved and made transparent, lyrics, and three global hotkeys (play/pause, previous, next).
Crosshair
A fixed crosshair at the center of the screen.
Overlay
Images and GIFs placed on top of everything.

## project
```

```
client\                 
  main.cpp              
  app.cpp / app.h       
  sections.cpp          
  crosshair.cpp/.h      
  overlay.cpp/.h        
  spotify.cpp/.h        
  videocfg.cpp/.h       
  color\                
  settings.h            
liquidDX11-main\        
resource\               
app.rc                  
build.bat               
```

---

## Crédits
- [Dear ImGui](https://github.com/ocornut/imgui)
- liquidDX11 
- FreeType
