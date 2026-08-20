# `src/architecture/`: the UDF kernel

Five header-only primitives that carry every piece of state in the app. They are
pure C++17 with no Qt, no QObject and no moc, so `core/` logic and its tests build
and run on the host with no Qt/SDL/sodium present.

Data flows one way. A source of truth owns state, composers purely derive from it,
the UI binds and renders, and the UI sends commands back to the source. Nothing
below `qml/` knows the UI exists. `docs/ARCHITECTURE.md` covers how the layers above
this kernel fit together; this file covers the primitives themselves.

## Pick the right primitive

| You have… | Use | File |
|---|---|---|
| a value that changes over time, observed by others | `Observable<S>` | `Observable.h` |
| state owned from a socket/timer/cache/setting | `StateSource<S>` (expose `state()`, mutate via `setState`) | `StateSource.h` |
| one value **purely derived** from other Observables | `Composer<Out, Ins...>` (one pure `transform`; no IO/events) | `Composer.h` / `Combiner.h` |
| a side effect driven by a state | `Controller<S>` (implement `apply`; `start()` idempotent; choose `stop()` teardown) | `Controller.h` |
| durable keyed storage | `Repository<K,V>` / `KeyedRepository<K,V>` (dumb, synchronous, thread-safe; wrap in a Source for reactive reads) | `Repository.h` |

Three more conventions are classes you write per feature rather than base classes
here. A **Coordinator** is a plain `QObject` service for imperative cross-cutting
commands over several sources; it re-exposes a child's `Observable` by reference and
never mirrors it, because a mirror is a second writer. A **Gateway** is an IO or
native boundary holding no domain state. A **Reducer**/**Mapper** is a pure free
function in `core/` for a `(state, event) -> result` decision or a domain-to-UI shape.

## Semantics that matter

- **`Observable`** is hot and always has a value. `set`/`update` notify only on
  change (`operator==`), so `S` must be equality-comparable.
  `subscribe(cb, emitCurrent=true)` replays the latest to a new subscriber. The
  returned `Subscription` is a move-only RAII unsubscribe handle that is safe in
  either destruction order. `subscribe` is `const` so a `Controller` holding a
  `const Observable&` can still attach.
- **`Combiner`/`Composer`** compute once eagerly at construction, so a consumer
  never observes a stale initial value, and recompute on any upstream change. Keep
  the `transform` a free function so it is testable in isolation. They are
  non-movable (they capture `this`); own one as a member or via `make_unique`.
- **`StateSource`** exposes `state()` read-only and mutates only through the
  protected `setState`. `start()`/`stop()` are opt-in; most sources here are
  process-scoped and simply live for the lifetime of the app.
- **`Controller`** applies the current value on `start()` and is idempotent.
  `stop()` defaults to tearing down the subscription, and is overridable for the
  deliberate cases that must survive a restart.

## Threading and the hot path

Notifications fire on the thread that called `set`. When binding to the UI from a
background thread, marshal to the GUI loop (`Qt::QueuedConnection` /
`QMetaObject::invokeMethod`).

Do not route the input hot path through this kernel. The path from an input event
to the `sendto` call stays plain, allocation-free C++.

## Tests

`tests/test_kernel.cpp` exercises all five primitives. The probes alongside it
(`StateSourceProbe.h`, `ComposerProbe.h`, `ControllerProbe.h`) capture emission
sequences rather than just final values, and `RepositoryContract.h` runs the eight
property tests every concrete repository must pass.
