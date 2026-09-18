# Open issues

- `RtxStressPassTest.theHoldComesToWhatWasAskedFromTheCardsOwnRate` fails on an unchanged tree when the card idles at a low clock (P4, 390 MHz): the 4 ms hold reads 5.2 ms and the 8 ms hold 10.1 ms, past the quarter the test allows. It passes with the card warm, and in the gate.
