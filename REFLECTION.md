# Reflection

## Design decisions

We sent sensor readings and commands as JSON instead of one value per topic.
This kept related readings together and let a single command topic control
both the servo and the LED. We also added a separate acknowledgement topic,
so the dashboard shows what the hardware actually did, not just what was
requested. The ESP32 only talks to the local Mosquitto broker, and Node-RED
on the Pi forwards the data to ThingsBoard. This means the board never needs
cloud credentials, and the cloud side can change without reflashing it.

## Challenges

The hardest problems were about power and networking rather than code. Every
time the servo moved, the Raspberry Pi restarted, because both boards were
powered from the same laptop and the motor's start-up current pulled the
voltage down. We moved the Pi to its own charger and made the servo turn in
small steps. The Pi also would not join the iPhone hotspot until Maximize
Compatibility was turned on. The Arduino Serial Monitor showed garbage for a
while, and reading the port with screen proved the board was fine. The tutor's
Edge Impulse model gave about 0.33 for every class, so we retrained it on 900
samples, which raised its accuracy from 40 % to 97.2 %.

## What we would improve

We would give the servo its own 5 V supply and add MQTT authentication and
TLS instead of anonymous access on port 1883. A fixed IP address for the Pi
would also help, since the hotspot kept changing it. Finally, we would store
the history in a database and let the system act on its own, for example
moving the servo when the AI detects a hot or humid condition.