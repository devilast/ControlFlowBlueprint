# Control Flow BP

Build gameplay sequences in Blueprints that read top to bottom. Queue steps, waits, delays, branches, parallel tracks and loops, with timeouts, clean cancellation and a live debugger.

Control Flow BP lets you build a gameplay sequence as a list of steps that run in order. Create a flow, queue the steps, execute it. Each step is an ordinary function or custom event on your Blueprint, picked from a dropdown on the node, so the graph reads top to bottom like a script.

```text
Create Control Flow (Owner = self)
  → Queue Step   PlayIntro     Wait: the flow continues when PlayIntro calls Continue Step
  → Queue If     IsBossLevel   Then or Else queues the steps of the case that runs
  → Queue Step   Delay         2 seconds of game time
  → Queue Step   Cleanup       Function: runs, and the flow moves on
  → Execute Flow Async         On Completed / On Cancelled / On Step Failed
```

## The problem it solves

Games are full of sequences: call the elevator, wait for the player, ride up, wait two seconds, open the door. Built from Delay nodes, timers, Bind and Unbind wiring and IsRunning booleans, they work until the player triggers one twice, the actor is destroyed halfway through, the game is paused, or a step never finishes.

Control Flow BP covers each of those cases. A named flow can tell you it is already running, a flow stops when its owner is destroyed, delays and timeouts stand still while the game is paused, and a step that never finishes times out with an error that names it.

## Quick start

1. In any Blueprint, add **Create Control Flow**. Owner defaults to `self`.
2. From its Flow pin, add **Queue Step**. Leave Step Kind on Function and pick a function from the dropdown, or choose **[Create a matching Function event]** to add one.
3. Add another **Queue Step**, set Step Kind to **Delay** and Seconds to `2`.
4. Finish the chain with **Execute Flow**, or **Execute Flow Async** to get pins for how the flow ended.

## Nodes

| Node | What it does |
| --- | --- |
| **Create Control Flow** | Creates an empty flow owned by an object, usually `self`. |
| **Queue Step** | Queues one step. Pick the step kind, and the Function dropdown lists only the functions and custom events that fit it. |
| **Queue If** | Asks a Condition function and runs Then or Else. Then and Else take a Control Flow and queue the steps of their case. |
| **Queue Wait Until** | Waits until a function returns true, checked every frame or at an interval you set. |
| **Queue Wait For Event Dispatcher** | Waits for any event dispatcher, Blueprint or C++ such as On Destroyed, with no Bind or Unbind wiring. |
| **Queue Repeat** | Runs an iteration a set number of times. |
| **Queue For Each** | Runs an iteration once for each item of any array. |
| **Execute Flow** / **Execute Flow Async** | Starts the flow. The async node has On Completed, On Cancelled and On Step Failed pins. |

Every Queue node returns the flow on its Flow pin, so the nodes chain.

## Step kinds

Queue Step works out the kind from the parameters of the function or custom event, and its dropdown only offers the ones that fit the kind you picked. Add or remove that parameter on the event and the node follows it to the new kind.

| Step kind | The function or custom event takes | What happens |
| --- | --- | --- |
| Function | nothing, or an Instanced Struct payload | It runs and the flow moves on. |
| Wait | a Control Flow Step Handle | The flow waits until you call Continue Step or Fail Step on the handle, or the step times out. |
| Sub Flow | a Control Flow | You queue steps onto the sub-flow, and they run as one step. |
| Switch | a Control Flow Switch | Add cases with Add Case, then pick one with Select Case or Select Case By Name. |
| Parallel | a Control Flow Parallel | Add tracks with Add Track. They run together, started in order or in random order. |
| Race | a Control Flow Parallel | Like Parallel, but the first track to finish wins and the rest are cancelled. |
| Loop | a Control Flow Loop | Queue each iteration's steps onto Get Body and call Continue Looping to run another. Max Iterations caps it. |
| Delay | no function | The flow waits a number of seconds of game time. |

## Features

### Mistakes show up when you compile

The dropdowns only offer functions with the right parameters, and a function with the wrong signature is a compile error instead of a surprise at runtime. Rename a function, custom event or event dispatcher and every node that uses it follows the new name. The **[Create a matching ...]** entry in each dropdown adds a function or event with the right parameters in a single click.

### Safe by default

- A flow belongs to its owner. When the owner is destroyed the flow is cancelled, so nothing fires on a dead actor.
- Cancel a flow at any point. Give a flow a name and you can find it, check whether it is running, or stop it by that name, without keeping it in a variable.
- Every waiting step has a timeout, set per flow and overridable per step, so nothing waits forever without telling you.
- Choose what a failed step does: retry it, carry on, or stop the flow. Get Attempt tells the step which try it is on.
- Delays and timeouts run on game time and stop while the game is paused. Flows owned by widgets keep running, so a pause menu can still count down.

### Bigger flows without the spaghetti

Group steps into sub-flows, switch on a case by key or by name, run parallel tracks in order or in random order, race tracks so the first to finish wins, and loop with Continue Looping and an optional Max Iterations limit. Pass data with per-step payloads (Instanced Struct) and with flow variables, which every step, sub-flow and case of a flow can read.

### See exactly what is running

- The **Control Flow Debugger** (Tools > Debug) shows every running and recent flow as a graph, with each step's state, timing and details, and a preview of the steps still to come.
- During Play In Editor, live bubbles on your Blueprint nodes show which steps are running, waiting, finished or failed.
- Right-click a Queue node for **Show in Control Flow Debugger**, or **Break When This Step Runs** to stop the Blueprint debugger at the start of that step.
- Errors appear in the Play In Editor message log with a link to the node that queued the step, and every step has a readable path such as `Rocket.PrepareLaunch.Countdown`.
- `showdebug ControlFlow` draws the running flows on screen, step tracing logs every step as it starts and finishes, and every step shows up as a timing region in Unreal Insights.

### Data-driven when you need it

**Queue Step By Name** (Control Flow > Queue > Advanced) runs functions named in data, so designers can reorder or extend a sequence such as a tutorial from the Details panel without touching the graph. A misspelled name is reported as soon as the step is queued.

## Console commands

| Command | What it does |
| --- | --- |
| `ControlFlowBP.Debugger` | Opens the Control Flow Debugger. |
| `ControlFlowBP.List` | Lists running and recently finished flows with the step each one is on. |
| `ControlFlowBP.Dump [name, wildcard or #id]` | Prints the running steps, queued steps and recent history of matching flows, or of every flow. |
| `ControlFlowBP.Cancel <name, wildcard or #id>` | Cancels matching running flows. |
| `ControlFlowBP.Trace 1` | Logs every step as it starts and finishes. `2` also logs each step as it is queued. |
| `ControlFlowBP.BreakOnStep <pattern>` | Pauses the Blueprint debugger when a step whose path matches is about to run, for example `Encounter.Waves#*.Spawn*`. |
| `showdebug ControlFlow` | Draws the running flows on screen. |

| Setting | Default | What it controls |
| --- | --- | --- |
| `ControlFlowBP.HistorySize` | 256 | How many finished steps each flow remembers for the debugger, Dump Flow and the node bubbles. |
| `ControlFlowBP.RecentFlows` | 32 | How many finished flows are kept for the debugger, the overlay and the editor. |
| `ControlFlowBP.DumpOnFailure` | On | Dumps a flow to the log when a step times out, hits Max Iterations or aborts the flow. |
| `ControlFlowBP.ScreenErrors` | On | Also prints errors on screen. Never in Shipping builds. |

## Requirements

- Unreal Engine 5.8 on Windows, which is what it is tested on. The code needs 5.5 or later; 5.5 to 5.7 have not been tested yet.
- The ControlFlows plugin that ships with Unreal Engine. It is enabled automatically with this plugin.

## Installation

1. Copy this folder into your project's `Plugins` folder, so the descriptor ends up at `YourProject/Plugins/ControlFlowBP/ControlFlowBP.uplugin`.
2. Open the project. When Unreal asks to rebuild the missing modules, click **Yes**. Building from source needs a C++ toolchain, such as Visual Studio with the Game development with C++ workload.
3. Check that **Control Flow (Blueprint)** is enabled under Edit > Plugins.

## Using it from C++

The nodes call the public API of `UControlFlowBP` in `ControlFlowBP.h`, which is exported for other modules. Add `"ControlFlowBP"` to your module's dependencies in its `.Build.cs` to call it from C++.

## Where it fits

Use it for anything that runs as a sequence of steps: interactive objects, tutorials, quest steps, scripted events, timed challenges and UI flows. It works in actor, component, widget and object Blueprints. Parallel tracks run on the game thread and take turns while they wait.

## Author

Created by Igor Nazarov.
