# MegaGus

This is the MEGA65 core (`development`) with **MegaGus** added: a 32-voice wavetable sound
engine in the spirit of the Gravis Ultrasound, with 512 KB of sample RAM, per-voice pitch,
volume, panning and looping, and hardware mixing at 48 kHz. See [docs/megagus.md](docs/megagus.md)
for the register map and its state. The rest of this file is the MEGA65 core's own README.

# MEGA65 = Enhanced C65 running in FPGA.

Please refer to the [main documentation index](https://github.com/MEGA65/mega65-core/blob/master/docs/index.md)
for the index file of the main DOC documentation.  

You will optionally need the a MEGA65 ROM file, if you wish to use BASIC65, you can download diffs
against one original C65 ROM [here](https://files.mega65.org?id=MEGA65ROMdiff). If you are a
registered MEGA65 owner, you can also download [full ROMs](https://files.mega65.org?id=MEGA65ROM).

If you do not have such a file, the MEGA65 contains the free and open-source
OpenROM alternative.

# Downloads

Follow the following links to download the latest releases and development builds:

* [MEGA65R6 Release](https://files.mega65.org?id=CoreMEGA65R6norom)
* [MEGA65R3 Release](https://files.mega65.org?id=CoreMEGA65R3norom)
* [MEGA65R2 Release](https://files.mega65.org?id=CoreMEGA65R2)
* [Nexys4DDR Release](https://files.mega65.org/?id=CoreNEXYS4DDR)
* [Development Builds](https://builder.mega65.org/job/mega65-core/job/development/)

## Automated Build Server

The MEGA65 project maintains an automated build server called [builder](https://builder.mega65.org/).
Here you can find builds of core, tools, and manual.
