#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PackageManifest {
    pub name: Option<String>,
    pub authoritative: bool,
    pub has_content: bool,
    pub version: Option<i64>,
    pub mount_order: Option<i64>,
    pub require_version: Option<i64>,
}

pub fn parse_package_manifest(text: &str) -> PackageManifest {
    let mut manifest = PackageManifest::default();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let (key, value) = match line.rsplit_once(char::is_whitespace) {
            Some((key, value)) => (key.trim(), Some(value.trim())),
            None => (line, None),
        };
        let number = || value.and_then(|value| value.parse::<i64>().ok());
        match key.to_ascii_lowercase().as_str() {
            "authoritative" if value.is_none() => manifest.authoritative = true,
            "hascontent" if value.is_none() => manifest.has_content = true,
            "name" => manifest.name = value.map(str::to_owned),
            "version" => manifest.version = number(),
            "mountorder" => manifest.mount_order = number(),
            "requireversion" => manifest.require_version = number(),
            _ => {}
        }
    }
    manifest
}
