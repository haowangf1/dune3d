issue:
When using computationally expensive operations like CSG of arrays of bodies, they slow down things like unrelated sketching. This happens even when the expensive bodies are set as invisible. My current work-around is simplifying the expensive operations temporarily (like lowering the number of copied bodies in an array).

author:
The problem with implementing that the naive way is that switching groups or turning on the solid model would require modifying the document to update the solid model. That'd mess up undo/redo.
I have some changes gathering dust on branch somewhere with the ultimate goal of moving the solid model update to an asynchronous background thread.