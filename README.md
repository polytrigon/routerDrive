I've created a gidget to help the community with uploading files to their Shaper. It costs very little in parts (less than $15). This project will always be open source and free.RouterDrive is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Shaper Tools, Festool, or their parent companies.

## What is it?

RouterDrive is a simple program that you flash onto an ESP32S3 based board which you then plug into your Shaper Origin via USB. It allows you to wirelessly upload DXF (auto converts) + SVG files. Your Shaper sees it as a regular ol' thumbdrive. If you're lazy like me and can't be bothered to walk a thumbdrive from your computer over to the origin then this is the tool for you! (its the purple thing in the photo below).

## Why did you create this?

It started with a conversation on another thread about alternative ways to get files onto the Shaper Origin besides Shaper Hub. User Beau pointed out that there was a usbA port that you can plug a thumb drive into with svg files. This was cool but I'm lazy so I thought maybe there was a way to just upload over the air... Beau also pointed out that there was such a solution made for 3d printers. I decided to make a tool a little more tailored for the Origin!

## Bill of Materials:

- XIAO SEED ESP32S3 (with wifi), it'll cost about $8 or more if you order it from Amazon.
- USBA to USBc cable, I recommend something short like 6"
- Basic knowledge uploading files through Arduino IDE less intimidating than it sounds
- Little bit of 3M VHB Tape to stick it on your Shaper
- A computer (of course!)

## How's it work?

So once you've gotten RouterDrive connected to your wifi network + plugged into the Shaper Origin USB port you visit the url RouterDrive.local on a browser and you'll see the following interface. From here you can upload both SVG and DXF files. DXF files will automatically convert to SVG. Then you just hit "restart device" and once it reboots which takes a second or two the files will appear on the Shaper in the "+ import" folder.

## Quick note to the Shaper Devs

If anyone up in the sky is listening it would be amazing if you could get usbA 'nudge' working, that would be so sweet! But no mind, thanks for creating a great tool!

## How to Install + Setup RouterDrive

So you've gotten all the parts together and are ready to get this thing installed - it's done in 6 painfree steps. 

1. Setup Arduino IDE
2. Download the RouterDrive .zip file
3. Connect your ESP32S3 to your computer + flash it with RouterDrive 
4. Connect to your ESP32S3's wifi network + insert workshop wifi credentials
5. Plug RouterDrive into your Shaper Origin's USB port
6. Use the browser interface at routerdrive.local to upload files

## 1. Setup Arduino IDE

This is by the far the most daunting step for folks who have never futzed with Arduino IDE before but I assure you we will get through this together. If you've already got this part done feel free to skip ahead.
