// SPDX-License-Identifier: Apache-2.0

$version: "2"

namespace ovf.exec.model

@range(min: 1)
long StableId

@length(min: 1, max: 128)
@pattern("^[a-z][a-z0-9_-]*$")
string SymbolicName

@length(min: 1, max: 4096)
string AbsolutePath

@length(max: 4096)
string OptionalAbsolutePath

@length(max: 128)
string OptionalSymbolicName

@length(max: 256)
string Argument

enum ReadinessPolicy {
    LIFECYCLE_CHANNEL = "lifecycle_channel"
    PROCESS_STARTED = "process_started"
    SUPERVISOR_NOTIFICATION = "supervisor_notification"
    SUCCESSFUL_EXIT = "successful_exit"
    SOCKET_AVAILABLE = "socket_available"
    MOUNT_PRESENT = "mount_present"
}

enum ExecutionUnitKind {
    MANAGED_APPLICATION = "managed_application"
    SERVICE = "service"
    ONE_SHOT = "one_shot"
    MOUNT = "mount"
    EXTERNAL = "external"
}

enum ReplacementPolicy {
    REJECT_WHILE_BUSY = "reject_while_busy"
    QUEUE = "queue"
    SUPERSEDE_IF_SAFE = "supersede_if_safe"
}

enum FailureAction {
    HOLD_OBSERVED_CONFIGURATION = "hold_observed_configuration"
    ENTER_FALLBACK_MODE = "enter_fallback_mode"
    STOP_DOMAIN = "stop_domain"
    REQUEST_SYSTEM_RECOVERY = "request_system_recovery"
}

enum ConstraintKind {
    REQUIRES_MODE = "requires_mode"
    EXCLUDES_MODE = "excludes_mode"
}

list StableIds {
    member: StableId
}

list UserIds {
    member: Long
}

list Arguments {
    member: Argument
}

list SymbolicNames {
    member: SymbolicName
}

structure RetryPolicy {
    @required
    maxAttempts: Integer

    @required
    delayMs: Long
}

structure ExecutionUnit {
    @required
    id: StableId

    @required
    name: SymbolicName

    @required
    kind: ExecutionUnitKind

    @required
    bootstrap: Boolean

    @required
    executable: OptionalAbsolutePath

    @required
    arguments: Arguments

    @required
    nativeService: OptionalSymbolicName

    @required
    stopExecutable: OptionalAbsolutePath

    @required
    stopArguments: Arguments

    @required
    readiness: ReadinessPolicy

    @required
    startTimeoutMs: Long

    @required
    stopTimeoutMs: Long

    @required
    retry: RetryPolicy

    @required
    dependencies: StableIds

    @required
    exclusiveResources: StableIds
}

structure ModeReference {
    @required
    domain: StableId

    @required
    mode: StableId
}

structure ModeConstraint {
    @required
    kind: ConstraintKind

    @required
    other: ModeReference
}

list ModeConstraints {
    member: ModeConstraint
}

structure Mode {
    @required
    id: StableId

    @required
    name: SymbolicName

    @required
    units: StableIds

    @required
    constraints: ModeConstraints
}

@length(min: 1)
list Modes {
    member: Mode
}

structure RecoveryPolicy {
    @required
    action: FailureAction

    fallbackMode: StableId

    @required
    deadlineMs: Long
}

structure ExecutionDomain {
    @required
    id: StableId

    @required
    name: SymbolicName

    @required
    initialMode: StableId

    @required
    replacement: ReplacementPolicy

    @required
    recovery: RecoveryPolicy

    @required
    modes: Modes
}

@length(min: 1)
list ExecutionUnits {
    member: ExecutionUnit
}

@length(min: 1)
list ExecutionDomains {
    member: ExecutionDomain
}

structure DinitBackend {
    @required
    backendLibrary: AbsolutePath

    @required
    systemRecoveryService: SymbolicName

    @required
    controlSocket: AbsolutePath

    @required
    servicesDirectory: AbsolutePath

    @required
    logBufferSize: Long

    @required
    mountExecutable: AbsolutePath

    @required
    unmountExecutable: AbsolutePath

    @required
    nativeServices: SymbolicNames
}

structure Persistence {
    @required
    journal: AbsolutePath

    @required
    maximumRecordSize: Long

    @required
    synchronize: Boolean
}

structure CoordinatorEndpoint {
    @required
    socket: AbsolutePath

    @required
    queueCapacity: Integer

    @required
    workerCount: Integer

    @required
    connectionCapacity: Integer

    @required
    maximumMessageSize: Long

    @required
    observationUids: UserIds

    @required
    mutationUids: UserIds
}

structure ExecutionPlatform {
    @required
    dinit: DinitBackend

    @required
    persistence: Persistence

    @required
    coordinator: CoordinatorEndpoint
}

structure ExecutionDeployment {
    @required
    deploymentVersion: Integer

    @required
    generation: StableId

    @required
    units: ExecutionUnits

    @required
    domains: ExecutionDomains

    @required
    platform: ExecutionPlatform
}
