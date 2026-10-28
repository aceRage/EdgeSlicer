# Single Extruder Multimaterial Printing in OrcaSlicer

## Introduction

In this guide, we will explain the parameters for single extruder multimaterial printing in OrcaSlicer. The whole process and parameters are same as PrusaSlicer as OrcaSlicer is based on PrusaSlicer.
OrcaSlicer has some additional features and minor differences though. We will explain them in this guide.

## Prime tower

The prime tower is a structure that is printed before the actual print to ensure that the nozzle is primed with the correct filament. It helps to prevent oozing and stringing during the print. The prime tower can be customized in various ways, such as its size, shape, and position.

## Wipe tower type

Printer setting (Multimaterial page, Advanced) that chooses which wipe tower implementation builds the prime tower.

- **Type 1**: the tower Bambu Lab printers use. It suits printers that cut the filament and purge the old one into the tower. Bambu Lab printers always use Type 1 and do not show this option.
- **Type 2** (default): the tower for MMU, ramming and tool-changer setups. It is the safe choice for every other printer and is what they sliced with before this option existed. Ramming, cone walls, tower rotation, priming and the other Type 2 tower options are only shown with Type 2.

## Ramming

Ramming is a technique used to push the filament through the nozzle to ensure that it is primed and ready for printing. It can be adjusted in terms of speed and distance to optimize the priming process.

## Manual filament change

Manual filament change is a feature that allows the user to change the filament during the print. This can be useful for multi-material prints or when changing colors. The user can specify the position and timing of the filament change, as well as the speed and distance of the ramming process.
