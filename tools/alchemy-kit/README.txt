reAL / ALchemy comparison kit
=============================

This runs the same thousands of DirectSound and EAX calls twice on your PC, once through ALchemy's dsound.dll and once
through reAL's, and saves every answer. Comparing the two shows every place where reAL answers differently from
ALchemy. Nothing is installed and nothing outside this folder is changed.

Run it on a Windows PC where ALchemy works (its software engine, as games get it there).

1. Unpack the whole kit into a folder of its own, for example C:\alchemy-kit.
2. Copy ALchemy's dsound.dll into the ALchemy folder inside it (the one games get from ALchemy).
3. Close games and anything else using sound, so the runs aren't disturbed.
4. Double-click run.bat. It takes about 10 minutes. You may hear short bursts of noise or clicks.
   If Windows SmartScreen warns about run.bat or dsfuzz.exe: "More info", then "Run anyway".
5. When it says Done, send results.zip.

What it does: dsfuzz.exe makes DirectSound objects and buffers and calls their methods with values from the edges
(odd sizes, limit values, infinite floats, EAX properties of every version and size), printing what each call
returns. Windows loads the dsound.dll that sits next to the program, so the copy in the ALchemy folder runs under
ALchemy and the copy in the reAL folder under reAL. results\system.txt notes your Windows version, sound device
and which ALchemy dsound.dll was used. The reAL in the kit runs with fixes=false (reAL.ini), so it's compared as
ALchemy's twin.

If a run crashes or hangs, dsfuzz.exe notes it in the results and the script goes on with the next batch.

More passes, if asked for them:
- run2.bat runs the same calls one seed per process, so a crash loses one seed and not 25. It takes an hour or more
  and writes results2.zip.
- run3.bat runs alprobe.exe, DirectSound and EAX case by case, in a few minutes, and writes results3.zip.
- run4.bat runs applyprobe.exe under ALchemy, reAL and Windows' own DirectSound: a tone through a 3D buffer, moved
  away with each dwApply value, the level the speakers get recorded (the default output's loopback). Leave the
  volume up and nothing else playing; it takes about three minutes and writes results4.zip.
- run5.bat runs levelprobe.exe the same way: how loud a tone comes out on each channel for 2D buffers at several
  pans and 3D buffers in several places, about two minutes, results5.zip.
- run6.bat runs levelprobe.exe noise: pink noise, 3D every 15 degrees around and at five elevations, each channel's
  level in three bands, about three minutes, results6.zip.
Each pass writes its own folder and zip, so running one doesn't touch another's results.
