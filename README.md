# WristQuest

**A game-controller glove for carpal tunnel prevention and early-stage care**

## Problem statement

Carpal tunnel syndrome (CTS) is compression of the median nerve at the wrist,
causing pain, numbness and tingling in the hand. People who type, use a mouse
or do repetitive hand work for hours are especially exposed, and women are
more prone to CTS than men. By the time symptoms are obvious, the condition
has already set in. Two gaps allow this:

1. **No early intervention.** Most people do nothing for their wrists until
   pain starts, and those at the onset stage have little to stop it
   progressing.
2. **No real-time warning.** Nothing tells a person when their wrist movement
   has crossed from healthy into excessive.

## Proposed solution

WristQuest is a sensor glove that aims to prevent CTS in healthy users and
stop it progressing in people at the onset stage, using gameplay as the
medium to make the whole process more engaging. The glove maps wrist
movements to game controls, allowing any 2D game to be played through
controlled wrist motion. These wrist movements help users prevent CTS and
keep early-stage symptoms from progressing. The glove also vibrates when the
user makes too many wrist movements, prompting a rest.

## How it works

1. Sensors on the glove read wrist movement.
2. The microcontroller converts each movement into a game input.
3. The computer receives the input as a normal key press.
4. When activity crosses a set limit, the vibration motor alerts the user.

## Hardware

- Microcontroller: [e.g. Arduino / ESP32]
- Motion sensing: [your sensor]
- Force sensitive resistor: [what it measures]
- Vibration motor
- Glove and wiring

## Repository structure

| Folder        | Contents                              |
|---------------|---------------------------------------|
| `firmware/`   | Microcontroller code                  |
| `controller/` | Maps sensor data to game input        |
| `hardware/`   | Circuit diagram, parts list, photos   |
| `docs/`       | Pitch slides and references           |

## Team

- [Name] - [Role]
- [Name] - [Role]

## Disclaimer

WristQuest is a hackathon prototype, not a certified medical device. It does
not replace advice from a doctor or physiotherapist.

## License

MIT
