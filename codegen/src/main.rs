// SPDX-License-Identifier: Apache-2.0

use minijinja::{Environment, Value, context};
use serde_json::{Value as JsonValue, json};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs;
use std::path::PathBuf;

const CPP_CONTRACT_TEMPLATE: &str = include_str!("../templates/cpp_contract.hpp.j2");
const CPP_DEPLOYMENT_TEMPLATE: &str = include_str!("../templates/cpp_deployment.hpp.j2");
const DINIT_BOOT_TEMPLATE: &str = include_str!("../templates/dinit_boot.j2");
const DINIT_SERVICE_TEMPLATE: &str = include_str!("../templates/dinit_service.j2");

fn cpp_type(name: &str) -> String {
    match name {
        "bool" => "bool".into(),
        "f32" => "float".into(),
        "f64" => "double".into(),
        "bytes" => "std::span<const std::byte>".into(),
        "string" => "std::string".into(),
        value => value.into(),
    }
}

fn uuid_bytes(value: &str) -> String {
    value
        .replace('-', "")
        .as_bytes()
        .chunks(2)
        .map(|pair| format!("0x{}", std::str::from_utf8(pair).expect("UUID is ASCII")))
        .collect::<Vec<_>>()
        .join(", ")
}

fn codec_type(name: &str, namespace: &str) -> String {
    match name {
        "bool" | "f32" | "f64" | "bytes" | "string" => cpp_type(name),
        value => format!("{namespace}::{value}"),
    }
}

fn application_error(method: &JsonValue) -> String {
    let errors = method["errors"].as_array().cloned().unwrap_or_default();
    if errors.is_empty() {
        "std::monostate".into()
    } else {
        format!(
            "std::variant<{}>",
            errors
                .iter()
                .filter_map(JsonValue::as_str)
                .collect::<Vec<_>>()
                .join(", ")
        )
    }
}

fn ordered_types(types: &[JsonValue]) -> Result<Vec<JsonValue>, String> {
    let by_name: BTreeMap<&str, &JsonValue> = types
        .iter()
        .map(|shape| {
            (
                shape["name"].as_str().ok_or("type has no name").unwrap(),
                shape,
            )
        })
        .collect();
    let mut ordered = Vec::new();
    let mut visiting = BTreeSet::new();
    let mut visited = BTreeSet::new();

    fn visit<'a>(
        name: &'a str,
        by_name: &BTreeMap<&'a str, &'a JsonValue>,
        visiting: &mut BTreeSet<&'a str>,
        visited: &mut BTreeSet<&'a str>,
        ordered: &mut Vec<JsonValue>,
    ) -> Result<(), String> {
        if visited.contains(name) || !by_name.contains_key(name) {
            return Ok(());
        }
        if !visiting.insert(name) {
            return Err(format!("recursive by-value type cycle involving {name}"));
        }
        let shape = by_name[name];
        let mut references = Vec::new();
        match shape["kind"].as_str() {
            Some("sequence") => references.push(shape["element"].as_str().unwrap_or_default()),
            Some("struct") => {
                for member in shape["members"].as_array().into_iter().flatten() {
                    references.push(member["type"].as_str().unwrap_or_default());
                }
            }
            _ => {}
        }
        references.sort_unstable();
        for reference in references {
            visit(reference, by_name, visiting, visited, ordered)?;
        }
        visiting.remove(name);
        visited.insert(name);
        ordered.push(shape.clone());
        Ok(())
    }

    for name in by_name.keys() {
        visit(name, &by_name, &mut visiting, &mut visited, &mut ordered)?;
    }
    Ok(ordered)
}

fn prepare(mut model: JsonValue) -> Result<JsonValue, String> {
    let namespace = model["namespace"]
        .as_str()
        .ok_or("model has no namespace")?
        .replace('.', "::");
    let types = ordered_types(model["types"].as_array().ok_or("model has no types")?)?;
    let object = model.as_object_mut().ok_or("model root is not an object")?;
    object.insert("cpp_namespace".into(), JsonValue::String(namespace));
    object.insert("ordered_types".into(), JsonValue::Array(types));
    for service in object
        .get_mut("services")
        .and_then(JsonValue::as_array_mut)
        .ok_or("model has no services")?
    {
        for method in service
            .get_mut("methods")
            .and_then(JsonValue::as_array_mut)
            .ok_or("service has no methods")?
        {
            let value = application_error(method);
            method
                .as_object_mut()
                .ok_or("method is not an object")?
                .insert("application_error".into(), JsonValue::String(value));
        }
    }
    Ok(model)
}

fn render(model: JsonValue) -> Result<String, Box<dyn std::error::Error>> {
    let mut environment = Environment::new();
    environment.add_filter("cpp_type", cpp_type);
    environment.add_filter("codec_type", codec_type);
    environment.add_filter("uuid_bytes", uuid_bytes);
    environment.add_template("cpp_contract.hpp", CPP_CONTRACT_TEMPLATE)?;
    let template = environment.get_template("cpp_contract.hpp")?;
    let rendered =
        template.render(context! { model => Value::from_serialize(&prepare(model)?) })?;
    Ok(rendered.trim_end().to_owned() + "\n")
}

fn parse_contract_arguments(arguments: &[String]) -> Result<(PathBuf, PathBuf), String> {
    if arguments.len() != 3 {
        return Err("usage: ovf_codegen <ir> --output <header>".into());
    }
    let input = PathBuf::from(&arguments[0]);
    if arguments[1] != "--output" {
        return Err("expected --output".into());
    }
    let output = PathBuf::from(&arguments[2]);
    Ok((input, output))
}

fn generate_contract(input: PathBuf, output: PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let model = serde_json::from_slice(&fs::read(input)?)?;
    let rendered = render(model)?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    if fs::read_to_string(&output).ok().as_deref() != Some(&rendered) {
        fs::write(output, rendered)?;
    }
    Ok(())
}

fn native_mapping(service: u64, instance: u64, entry: &JsonValue) -> String {
    format!(
        "service={service};instance={instance};element={};eventGroup={};major={};minor={};kind={};reliable={}",
        entry["id"].as_u64().unwrap_or_default(),
        entry["eventGroup"].as_u64().unwrap_or_default(),
        entry["major"].as_u64().unwrap_or_default(),
        entry["minor"].as_u64().unwrap_or_default(),
        entry["kind"].as_str().unwrap_or_default(),
        entry["reliable"].as_bool().unwrap_or_default()
    )
}

fn iceoryx2_event_mapping(base: &str, entry: &JsonValue) -> String {
    format!(
        "pattern=pubsub;service={base}/{};type={};payloadSize={};alignment={};history={};subscriberBuffer={};maxPublishers={};maxSubscribers={};safeOverflow={}",
        entry["name"].as_str().unwrap_or_default(),
        entry["type"].as_str().unwrap_or_default(),
        entry["payloadSize"].as_u64().unwrap_or_default(),
        entry["alignment"].as_u64().unwrap_or_default(),
        entry["history"].as_u64().unwrap_or_default(),
        entry["subscriberBuffer"].as_u64().unwrap_or_default(),
        entry["maxPublishers"].as_u64().unwrap_or_default(),
        entry["maxSubscribers"].as_u64().unwrap_or_default(),
        entry["safeOverflow"].as_bool().unwrap_or_default(),
    )
}

fn iceoryx2_method_mapping(base: &str, entry: &JsonValue) -> String {
    format!(
        "pattern=requestResponse;service={base}/{};requestType={};responseType={};requestPayloadSize={};responsePayloadSize={};alignment={};requestBuffer={};responseBuffer={};maxClients={};maxServers={};safeOverflow={}",
        entry["name"].as_str().unwrap_or_default(),
        entry["requestType"].as_str().unwrap_or_default(),
        entry["responseType"].as_str().unwrap_or_default(),
        entry["requestPayloadSize"].as_u64().unwrap_or_default(),
        entry["responsePayloadSize"].as_u64().unwrap_or_default(),
        entry["alignment"].as_u64().unwrap_or_default(),
        entry["requestBuffer"].as_u64().unwrap_or_default(),
        entry["responseBuffer"].as_u64().unwrap_or_default(),
        entry["maxClients"].as_u64().unwrap_or_default(),
        entry["maxServers"].as_u64().unwrap_or_default(),
        entry["safeOverflow"].as_bool().unwrap_or_default(),
    )
}

fn generate_cpp_deployment(
    plan_path: PathBuf,
    namespace: &str,
    output: PathBuf,
) -> Result<(), Box<dyn std::error::Error>> {
    let plan: JsonValue = serde_json::from_slice(&fs::read(plan_path)?)?;
    let instance = plan["instances"]
        .as_array()
        .and_then(|values| values.first())
        .ok_or("deployment plan has no instance")?;
    let routes = instance["providerRoutes"]
        .as_array()
        .filter(|values| !values.is_empty())
        .or_else(|| instance["consumerRoutes"].as_array())
        .ok_or("deployment plan has no routes")?;
    let route = routes.first().ok_or("deployment plan has no route")?;
    let provider_id = route["provider"]
        .as_str()
        .ok_or("route provider is missing")?;
    let provider = plan["providers"]
        .as_array()
        .into_iter()
        .flatten()
        .find(|value| value["id"] == provider_id)
        .ok_or("route provider is unknown")?;
    let mapping = &route["mappings"];
    let provider_profile = provider["profile"].as_str().unwrap_or_default();
    let numeric_mapping = mapping["service"].is_u64();
    let service = mapping["service"].as_u64().unwrap_or_default();
    let instance_number = mapping["instance"].as_u64().unwrap_or_default();
    let mut elements = Vec::new();
    let iceoryx2_base = format!(
        "{}/{}",
        mapping["service"].as_str().unwrap_or_default(),
        mapping["instance"].as_str().unwrap_or_default()
    );
    for (id, value) in mapping["elements"]
        .as_object()
        .ok_or("element mappings are missing")?
    {
        let (event, method) = if provider_profile == "iceoryx2" {
            if value.get("unsupported").is_some() {
                (String::new(), String::new())
            } else if value.get("notify").is_some() || value.get("get").is_some() {
                (
                    value
                        .get("notify")
                        .map(|entry| iceoryx2_event_mapping(&iceoryx2_base, entry))
                        .unwrap_or_default(),
                    value
                        .get("get")
                        .or_else(|| value.get("set"))
                        .map(|entry| iceoryx2_method_mapping(&iceoryx2_base, entry))
                        .unwrap_or_default(),
                )
            } else if value.get("requestType").is_some() {
                (
                    String::new(),
                    iceoryx2_method_mapping(&iceoryx2_base, value),
                )
            } else {
                (iceoryx2_event_mapping(&iceoryx2_base, value), String::new())
            }
        } else if let Some(name) = value.as_str() {
            (name.to_owned(), name.to_owned())
        } else if value.get("id").is_some() {
            match value["kind"].as_str().unwrap_or_default() {
                "event" | "fieldNotify" => (
                    native_mapping(service, instance_number, value),
                    String::new(),
                ),
                _ => (
                    String::new(),
                    native_mapping(service, instance_number, value),
                ),
            }
        } else {
            (
                value
                    .get("notify")
                    .map(|entry| native_mapping(service, instance_number, entry))
                    .unwrap_or_default(),
                value
                    .get("get")
                    .or_else(|| value.get("set"))
                    .map(|entry| native_mapping(service, instance_number, entry))
                    .unwrap_or_default(),
            )
        };
        elements.push(json!({
            "id": uuid_bytes(id),
            "event_mapping": event,
            "method_mapping": method,
        }));
    }
    elements.sort_by(|left, right| left["id"].as_str().cmp(&right["id"].as_str()));
    let service_entry = json!({
        "id": 0,
        "eventGroup": 0,
        "major": 1,
        "minor": 0,
        "kind": "method",
        "reliable": true,
    });
    let service_mapping = if numeric_mapping {
        native_mapping(service, instance_number, &service_entry)
    } else {
        format!(
            "{}/{}",
            mapping["service"]
                .as_str()
                .ok_or("service mapping is missing")?,
            mapping["instance"]
                .as_str()
                .ok_or("instance mapping is missing")?
        )
    };
    let view = json!({
        "namespace": namespace,
        "provider": provider["profile"],
        "max_endpoints": route["limits"]["maxEndpoints"],
        "max_operations": route["limits"]["maxOutstandingOperations"],
        "service_id": uuid_bytes(instance["serviceId"].as_str().ok_or("serviceId is missing")?),
        "instance_id": uuid_bytes(instance["instanceId"].as_str().ok_or("instanceId is missing")?),
        "max_payload_size": route["limits"]["maxPayloadSize"],
        "history_depth": route["limits"]["maxHistoryDepth"],
        "priority": route.get("priority").and_then(JsonValue::as_u64).unwrap_or_default(),
        "elements": elements,
        "service_mapping": service_mapping,
    });
    let mut environment = Environment::new();
    environment.add_template("cpp_deployment.hpp", CPP_DEPLOYMENT_TEMPLATE)?;
    let rendered = environment
        .get_template("cpp_deployment.hpp")?
        .render(context! { deployment => Value::from_serialize(&view) })?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(output, rendered)?;
    Ok(())
}

fn generate_dinit_services(
    model_path: PathBuf,
    output: PathBuf,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut model: JsonValue = serde_json::from_slice(&fs::read(model_path)?)?;
    let units = model["units"]
        .as_array_mut()
        .ok_or("execution model has no units")?;
    let service_names = units
        .iter()
        .map(|unit| {
            let id = unit["id"].as_u64().ok_or("execution unit id is missing")?;
            Ok((
                id,
                if unit["kind"] == "external" {
                    unit["nativeService"]
                        .as_str()
                        .ok_or("external unit native service is missing")?
                        .to_owned()
                } else if unit["kind"] == "managed_application" {
                    format!("ovf-app-{id}")
                } else {
                    format!("ovf-unit-{id}")
                },
            ))
        })
        .collect::<Result<std::collections::BTreeMap<_, _>, Box<dyn std::error::Error>>>()?;
    for unit in units.iter_mut() {
        let arguments = unit["arguments"]
            .as_array()
            .ok_or("execution unit arguments are missing")?;
        let mut command = vec![
            unit["executable"]
                .as_str()
                .ok_or("execution unit executable is missing")?
                .to_owned(),
        ];
        command.extend(
            arguments
                .iter()
                .map(|value| {
                    value
                        .as_str()
                        .ok_or("execution unit argument is not a string")
                        .map(str::to_owned)
                })
                .collect::<Result<Vec<_>, _>>()?,
        );
        let start = unit["startTimeoutMs"]
            .as_u64()
            .ok_or("execution unit start timeout is missing")?;
        let stop = unit["stopTimeoutMs"]
            .as_u64()
            .ok_or("execution unit stop timeout is missing")?;
        let stop_executable = unit["stopExecutable"].as_str().unwrap_or_default();
        let stop_arguments = unit["stopArguments"]
            .as_array()
            .ok_or("execution unit stop arguments are missing")?;
        let stop_command = if stop_executable.is_empty() {
            String::new()
        } else {
            std::iter::once(stop_executable.to_owned())
                .chain(
                    stop_arguments
                        .iter()
                        .map(|value| {
                            value
                                .as_str()
                                .ok_or("execution unit stop argument is not a string")
                                .map(str::to_owned)
                        })
                        .collect::<Result<Vec<_>, _>>()?,
                )
                .collect::<Vec<_>>()
                .join(" ")
        };
        let dependency_services = unit["dependencies"]
            .as_array()
            .ok_or("execution unit dependencies are missing")?
            .iter()
            .map(|dependency| {
                service_names
                    .get(
                        &dependency
                            .as_u64()
                            .ok_or("dependency id is not an integer")?,
                    )
                    .cloned()
                    .ok_or_else(|| "dependency service mapping is missing".into())
            })
            .collect::<Result<Vec<String>, Box<dyn std::error::Error>>>()?;
        let object = unit
            .as_object_mut()
            .ok_or("execution unit is not an object")?;
        object.insert("command".into(), JsonValue::String(command.join(" ")));
        object.insert("stop_command".into(), JsonValue::String(stop_command));
        object.insert(
            "dependency_services".into(),
            serde_json::to_value(dependency_services)?,
        );
        object.insert(
            "start_timeout_seconds".into(),
            JsonValue::from(start.div_ceil(1000).max(1)),
        );
        object.insert(
            "stop_timeout_seconds".into(),
            JsonValue::from(stop.div_ceil(1000).max(1)),
        );
    }
    let mut environment = Environment::new();
    environment.add_template("dinit_service", DINIT_SERVICE_TEMPLATE)?;
    environment.add_template("dinit_boot", DINIT_BOOT_TEMPLATE)?;
    fs::create_dir_all(&output)?;
    let bootstrap_services = units
        .iter()
        .filter(|unit| unit["bootstrap"].as_bool() == Some(true))
        .filter_map(|unit| unit["id"].as_u64())
        .filter_map(|id| service_names.get(&id).cloned())
        .collect::<Vec<_>>();
    let boot = environment
        .get_template("dinit_boot")?
        .render(context! { bootstrap_services })?;
    fs::write(output.join("boot"), format!("{}\n", boot.trim()))?;
    let service = environment.get_template("dinit_service")?;
    for unit in units {
        if unit["kind"] == "external" {
            continue;
        }
        let identifier = unit["id"].as_u64().ok_or("execution unit id is missing")?;
        let service_name = service_names
            .get(&identifier)
            .ok_or("execution unit service mapping is missing")?;
        let rendered = service.render(context! { unit => Value::from_serialize(&*unit) })?;
        fs::write(output.join(service_name), format!("{}\n", rendered.trim()))?;
        if unit["kind"] == "managed_application" {
            fs::write(
                output.join(format!("{service_name}.env")),
                format!("OVF_EXEC_APPLICATION_ID={identifier}\n"),
            )?;
        }
    }
    Ok(())
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let arguments = env::args().skip(1).collect::<Vec<_>>();
    if arguments.first().map(String::as_str) == Some("execution-dinit") {
        if arguments.len() != 5 || arguments[1] != "--model" || arguments[3] != "--output" {
            return Err(
                "usage: ovf_codegen execution-dinit --model <model> --output <directory>".into(),
            );
        }
        return generate_dinit_services(PathBuf::from(&arguments[2]), PathBuf::from(&arguments[4]));
    }
    if arguments.first().map(String::as_str) == Some("deployment-cpp") {
        if arguments.len() != 7
            || arguments[1] != "--plan"
            || arguments[3] != "--namespace"
            || arguments[5] != "--output"
        {
            return Err("usage: ovf_codegen deployment-cpp --plan <plan> --namespace <name> --output <header>".into());
        }
        return generate_cpp_deployment(
            PathBuf::from(&arguments[2]),
            &arguments[4],
            PathBuf::from(&arguments[6]),
        );
    }
    let (input, output) = parse_contract_arguments(&arguments)?;
    generate_contract(input, output)
}

fn main() {
    if let Err(error) = run() {
        eprintln!("ovf_codegen: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn radar_contract_is_deterministic_and_contains_the_public_surface() {
        let model: JsonValue =
            serde_json::from_str(include_str!("../../com/model/examples/radar.ovf-ir.json"))
                .expect("valid fixture");
        let first = render(model.clone()).expect("render succeeds");
        let second = render(model).expect("render succeeds");
        assert_eq!(first, second);
        assert!(first.contains("class RadarServiceProxy final"));
        assert!(first.contains("class RadarServiceOffer final"));
        assert!(first.contains("static constexpr ovf::com::Uuid id"));
    }

    #[test]
    fn empty_contract_matches_golden_file() {
        let model: JsonValue =
            serde_json::from_str(include_str!("../tests/fixtures/empty.ovf-ir.json"))
                .expect("valid fixture");
        let actual = render(model).expect("render succeeds");
        assert_eq!(actual, include_str!("../tests/golden/empty.hpp"));
    }
}
