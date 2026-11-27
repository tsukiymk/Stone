# Power, jobs, and system services

Service samples and persistently manages host state. Android tokens, listeners, objects, and actual component callbacks remain in Client's ART. Client queries use asynchronous snapshots; the UI thread does not wait for Windows system sampling.

## Power and battery

Power snapshots come from Windows power state and include supply, charging, battery level, and battery saver mode. Android battery thresholds come from image resources. The image dispatches initial sticky broadcasts and later changes. Unknown fields remain unknown; temperature, current, energy, and other values unavailable from Windows are not fabricated.

The thermal service follows Android's state without a thermal HAL: no Android thermal throttling is active (`THERMAL_STATUS_NONE`), and registered status listeners receive that initial state. This is not a measurement of Windows thermal pressure. Thermal headroom is unavailable (`NaN`); sensor telemetry and other thermal interfaces remain unsupported.

ART manages Java tokens, reference counts, and timeouts for supported partial wake locks. Service holds Windows power requests against registered Client process handles and cleans them up when the process exits. This does not provide full Doze, the Android power HAL, or system sleep policy.

## JobScheduler

Service owns queues, constraints, timing/backoff, persistence, and leases. Client's ART owns JobService binding and callbacks. Network, battery, storage, and idle constraints use supported host state.

Background wakeups use launch profiles bound to code, data, and image. Cold Service recovery within the current login session retains only jobs declared persistent. System boot, new logins, complete quotas, and all long-duration behavior are not fully implemented.

## Notifications and foreground services

Android channels, authorization, notification objects, and PendingIntent callbacks remain in ART. Service manages Windows delivery and supported recoverable routes. Foreground-service ID ownership, stop modes, and cancellation protection are independent of ordinary notification cancellation.

Some notifications and non-serializable actions require a live Client. Recoverable PendingIntents save only supported identity and Intent snapshots, never Java Binder, Application, or component objects. See [Windows integration](../windows/integration.md) for host delivery and recovery boundaries.

## Transient messages

Foreground text or custom Toasts use image ToastPresenter and Client software child windows, following queue, callback, cancellation, and token lifecycles. Transient messages do not enter Windows notification history and are distinct from background system notifications.

Complete system services, device management, privileged permissions, and arbitrary broadcast policies are outside the supported compatibility scope. Unconnected interfaces must fail explicitly.
