10 rem megagus: load a sample from disk and play it on a key
20 r=$efff000
30 if peek($efff200)<>77 or peek($efff201)<>71 then print "no megagus in this core":end
40 poke $efff204,129:poke $efff204,1
50 bload "kick.raw",p($e000000),r:rem signed 8-bit, 22050 hz, straight into the sample ram
60 n=8820:rem its length in bytes
70 f=int(22050*65536/48000):rem its own pitch
80 poke r+1,128:wpoke r+2,50000
90 wpoke r+4,f:poke r+6,0
100 wpoke r+10,0:poke r+12,0
110 wpoke r+13,n:poke r+15,0
120 print "space plays it, q ends"
130 getkey a$:if a$="q" then end
140 poke r,0:wpoke r+7,0:poke r+9,0:poke r,1:rem from the start, once
150 goto 130
