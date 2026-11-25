10 rem megagus: three voices play a chord from one sine wave
20 m=$e000000:r=$efff000
30 if peek($efff200)<>77 or peek($efff201)<>71 then print "no megagus in this core":end
40 poke $efff204,129:poke $efff204,1:rem reset the engine, then switch it on
50 for i=0 to 63:v=int(127*sin(i*3.14159265/32)):if v<0 then v=v+256
60 poke m+i,v:next:rem one cycle of a sine, 64 samples, at the start of the sample ram
70 for n=0 to 2:a=r+n*16:read hz,p
80 f=int(hz*64*65536/48000):rem 64 samples, hz times a second
90 b2=int(f/65536):f=f-b2*65536
100 poke a,0:rem stop the voice while it is set up
110 poke a+1,p:wpoke a+2,20000:rem pan, volume
120 wpoke a+4,f:poke a+6,b2:rem freq
130 wpoke a+10,0:poke a+12,0:rem start (of the loop): 0
140 wpoke a+13,64:poke a+15,0:rem end: 64
150 wpoke a+7,0:poke a+9,0:rem pos: 0; the write to its last byte sets it
160 poke a,3:rem run + loop
170 next
180 print "a key ends it":getkey a$
190 for n=0 to 2:wpoke r+n*16+2,0:next:rem volumes to 0: they ramp, no click
200 sleep 0.05:for n=0 to 2:poke r+n*16,0:next:rem then stop
210 data 261.63,48,329.63,128,392,208
