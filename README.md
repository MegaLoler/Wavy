# Wavy

A super fast 'n' simple audio player with an SDL based interface.

## Features

* Navigable audio waveform viewport.
* Can play any file format supported by `ffmpeg`.
* Fully keyboard controllable and/or fully mouse controllable.

## Explanation

### Interface

The interface consists predominantly of a view of the audio waveform.
The current playback position is indicated with a white vertical bar over the waveform view.
Time is measured along the bottom of the window.
There are two navigation rulers along the top of the window each divided into 10.
The top one is the global navigation ruler which spans the entire audio buffer.
The bottom one is the local navigation ruler which spans the current viewport.
Status flags are displayed in the title of the window.

### Opening audio

Load an audio file from the command line like so:
```bash
wavy something.mp3
```

### Playback navigation

Toggle between playing and paused with the space key.
Toggle between looping and non-looping with the `L` key.
Toggle playback direction with the `R` key.
Both of these are enabled by default when a file is opened.

Left-click or drag at any time anywhere on the audio waveform to jump the audio cursor to that position.
Alternatively press any key `0` through `9` in order to jump to the indicated position on the local navigation ruler.
Hold shift while pressing the key in order to jump to that position on the global ruler instead.
Press the return key in order to jump to the beginning of the audio buffer.

Alternatively use the arrow keys to move the playback cursor left or right.

### Selecting an audio region

Right-click and drag on the audio waveform in order to select a region of audio.
Alternatively click without dragging at two positions in order to select that region.
Another option is to hold the control key while sequentially pressing any two of the number keys in order to select the region between those two indicated positions on the local navigation ruler.
Hold control _and_ shift while pressing the numbers in order to use the positions indicated by the global navigation ruler instead.

Once a region is selected right-click in order to move one of the poles of the region selection (the end closest to where you initially click).

Double right-click to clear the region selection.
Alternatively press the `S` key to clear the selection.

Alternatively hold the control key while using the left and right arrow keys to adjust the first pole of the selected region.
Likewise uset he up and down arrow keys to adjust the second pole.

### Viewport navigation

Middle-click and drag in order to pan the viewport left or right.
Use the scroll wheel in order to zoom in and out.
Double middle-click in order to zoom all the way out in order to see the whole buffer.

Alternatively hold down the option key and press any of the number keys in order to set the start of the viewport to that position indicated by the local navigation ruler.
Naturally hold down the shift _and_ option key while pressing the number in order to use the position indicated by the global navigation ruler instead.
Hold down the control _and_ option key (and possible the shift key too) in order to set the _end_ of the viewport to the indicated position.

Note that its possible to reverse the viewport so that the audio runs from right to left instead of left to right by setting the beginning of the viewport to a position after the ending of the viewport.

Alternatively hold down the option key while using the left and right arrow keys to pan the viewport left and right.
Likewise use the up and down arrow keys to zoom in and out.
