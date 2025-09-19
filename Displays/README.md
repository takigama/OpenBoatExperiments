# Displays

So this will be the head of the remote displays code. These will all probably be esp32 based LCD screens (perhaps with touch screeen) with a daughter board for some buttons and a case to keep it all nice and water tight.

The idea is that the Sensors will feed into some form of simple hub (mqtt based) and the displays will have a button to bring up a menu that you'll then be able to choose which display you want... i.e. compass, speed, location... blah blah blah. Ideally I want to be able to send a simple vector map to one of the displays, but im not sure if I can export that from opencpn or anything?

## Tested so far....

So I have a tonne of displays that i've collected over the years, the one I've tested is the display thats quite common called the CYD (cheep yellow display) which is abundant on many a cheap site and is a very cheap display. Theres lots of info out there for the display for running lvgl and the like. But its small, 2.4" is too small for a marine display (see https://randomnerdtutorials.com/programming-esp32-cyd-cheap-yellow-display-vs-code/?unapproved=1097190&moderation-hash=9008df86d627921fde26b91361bac962#comment-1097190).

I decided to buy a another display to test out and found one set that looks like it has a good set of displays covering a decent range of sizes, specifically the ViewSmart Displays (https://github.com/VIEWESMART). They have 4, 4.3, 5 and 7. I got a 7 and its quite huge, too huge really. Its about as big as a 7" tablet, which is probably in the ball park of a marine plotter and really not what I was after for displays. However, I did have an idea of an on-arm display for controlling the boat and having a display near by.