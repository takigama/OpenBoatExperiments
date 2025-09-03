# OpenBoat

Getting back into sailing after a number of years and my electronics itch is wanting me to make an entirely open-source sailing boat computer system.

So far this is a dump of my ideas and nothing more.

# Components

## connectivity

Everything will be connected via wifi using ESP32's. There will be some form of router required (preferably something openwrt based that can handle running gpsd and mqtt

## sensors

First thing im designing is a boat that will have an esp32 + accelerometer + gyro + magnetometer + gps... This should be easy enough to do quite cheaply (around $20 each). They'll publish all their data to gpsd and mqtt.

Second will be an AIS receiver (or two) based on dAISy or RTL-SDR

## displays

Theres quite a number of 5-10" lcd's with onboard esp32's that i'll be targetting, and they are quite cheap too. I'll probably be coding those from scratch. These will connect back to some central machine on the boat that tells them what to display. They'll be 3 buttons on each display, one for paging, one for silencing alarms and one for something else. Each display will be identified by its mac address (or some chip id) and all devices will run a single firmware that gets OTA updated from the central machine.

## central machine

Basically something that can run a web interface that will control the displays and consume information. At the moment, this might be a nanopi friendly elec with hdmi output that'll come to a normal 24" tv in the boat. Be capable of running opencpn and the web control interface (probably node as a docker container).

Hopefully the tv will be somewhere you might be able to swivel it so you can see it from the tiller/wheel.

## opencpn exporting...

it would be nice to be able to select a course in opencpn then be able to export details back to displays.. For eg, if you're off-track, something similar to a aircraft nav display (HSI) would be interesting... can show both deviation from heading and deviation from line at the same time.

## other sensors.

a 3d printed wind speed/direction sensor would be an interesting addition.

There is one diy depth sounder, but we'll see...

Most boats already have some form of speed indicator, reading it from an ESP32 should be pretty trivial.. The rest of the information about speeds and directions should be able to be gotten from gps and wind speed/direciton sensor.

Direction of rudder

## types of displays

HSI like NAV on an airplane would be very cool

Compass that shows boat heading (i.e. where the front is pointed) and any deviation for the boat travel (i.e. are we sidling) - plus other pointers to things like closest land heading, other boats, etc.

Small vector map dislpay would be nice, but may be hard to get vector maps from anywhere that is useful, also showing any details about other boats from AIS.

Some form of radio sensor

volts/amps, etc

Autopilot stuff...


## Autopilot....

There's already fenix for open AP, though i had an idea for a different control mechism that doesnt use a linear actuator.

## The wearable...

So one other thing I'd love is a wrist wearable 7" display similar to the other displays.

## other projects...

openplotter and fenix are interesting and possibly useful for some of the stuff, but i dont like RPI's for this kind of thing, the lack of on-board storage and everything on the SD card i've had issues with in other mobile things.

## Power control.

One other facet I want to include in all components it the ability for the ESP's to "disable" stuff (i.e. turn off the GY-86, gps or several of the displays). this is useful when anchored cause you really only need one gps to be sending data with no real displays and if you get a sudden movement, power everything back up again.

## AI Detection

Another interesting concept to explore is frigate. Frigate is an NVR thats existed for a long time but uses the Coral TPU (and other inference capable devices) with tensor flow to do image recog (mine at home has been trained to recognise me, my partner and both our dogs very accurately). A "fork" of frigate that was designed to do image recognition would be very interesting on boat.... the sort of thing im talking about it:
 - water navigation marks
 - other boats
 - people overboard
 - land and other water marks.
... and other things?

AI could also be extended to the autopilot intreesting ways when combined with all the sensors available.

