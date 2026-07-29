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

@length(max: 256)
string Argument

enum ReadinessPolicy {
    REQUIRED = "required"
    PROCESS_STARTED = "process_started"
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

list Arguments {
    member: Argument
}

structure RetryPolicy {
    @required
    maxAttempts: Integer

    @required
    delayMs: Long
}

structure Application {
    @required
    id: StableId

    @required
    name: SymbolicName

    @required
    executable: AbsolutePath

    @required
    arguments: Arguments

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
    applications: StableIds

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
list Applications {
    member: Application
}

@length(min: 1)
list ExecutionDomains {
    member: ExecutionDomain
}

structure DinitBackend {
    @required
    controlSocket: AbsolutePath

    @required
    servicesDirectory: AbsolutePath

    @required
    logBufferSize: Long
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
    applications: Applications

    @required
    domains: ExecutionDomains

    @required
    platform: ExecutionPlatform
}
