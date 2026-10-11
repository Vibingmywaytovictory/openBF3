use std::path::{Path, PathBuf};
use std::sync::Arc;

use superbundle_bf3::{Layout, Toc, deobfuscate, parse_layout, parse_package_manifest, parse_toc};

use super::Bf3Error;
use super::cas::CasStore;
use super::file::{FileCache, read_whole, resolve};
use super::mount::LevelMount;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bf3Package {
    pub name: String,
    pub data_dir: PathBuf,
    pub authoritative: bool,
    pub version: Option<i64>,
    pub require_version: Option<i64>,
    pub mount_order: i64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bf3Level {
    pub id: String,
    pub superbundle: String,
    pub expansion: Option<u8>,
    pub base_toc: Option<PathBuf>,
    pub patch_toc: Option<PathBuf>,
}

#[derive(Clone, Debug)]
pub(crate) struct SuperbundleSite {
    pub(crate) name: String,
    pub(crate) base_toc: Option<PathBuf>,
    pub(crate) patch_toc: Option<PathBuf>,
    pub(crate) delta: bool,
}

pub struct Bf3Install {
    root: PathBuf,
    data_dir: PathBuf,
    packages: Vec<Bf3Package>,
    layout: Layout,
    sites: Vec<SuperbundleSite>,
    cas: CasStore,
    files: FileCache,
}

impl Bf3Install {
    pub fn open(root: &Path) -> Result<Arc<Self>, Bf3Error> {
        let not_an_install = || Bf3Error::NotAnInstall {
            root: root.to_path_buf(),
        };
        let data_dir = resolve(root, "Data").ok_or_else(not_an_install)?;
        let base_layout = resolve(&data_dir, "layout.toc").ok_or_else(not_an_install)?;
        resolve(&data_dir, "cas.cat").ok_or_else(not_an_install)?;

        let (mut packages, patch) = mounted_packages(root);
        let patch_dir = patch.as_ref().map(|patch| patch.data_dir.clone());
        let layout_path = patch_dir
            .as_ref()
            .and_then(|dir| resolve(dir, "layout.toc"))
            .unwrap_or(base_layout);
        let layout = deobfuscate(read_whole(&layout_path)?)
            .and_then(|plain| parse_layout(&plain))
            .map_err(|error| Bf3Error::format(layout_path.display(), error))?;

        let base_dirs = std::iter::once(data_dir.clone())
            .chain(packages.iter().map(|package| package.data_dir.clone()))
            .collect::<Vec<_>>();
        let sites = layout
            .superbundles
            .iter()
            .map(|decl| {
                let toc = format!("{}.toc", decl.name);
                SuperbundleSite {
                    name: decl.name.clone(),
                    base_toc: base_dirs.iter().find_map(|dir| resolve(dir, &toc)),
                    patch_toc: patch_dir.as_ref().and_then(|dir| resolve(dir, &toc)),
                    delta: decl.delta,
                }
            })
            .collect();

        let catalog_dirs = patch_dir
            .iter()
            .cloned()
            .chain(std::iter::once(data_dir.clone()))
            .collect::<Vec<_>>();
        let cas = CasStore::open(&catalog_dirs)?;

        packages.extend(patch);
        Ok(Arc::new(Self {
            root: root.to_path_buf(),
            data_dir,
            packages,
            layout,
            sites,
            cas,
            files: FileCache::default(),
        }))
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn data_dir(&self) -> &Path {
        &self.data_dir
    }

    pub fn packages(&self) -> &[Bf3Package] {
        &self.packages
    }

    pub fn layout(&self) -> &Layout {
        &self.layout
    }

    pub fn cas(&self) -> &CasStore {
        &self.cas
    }

    pub fn superbundle_names(&self) -> impl Iterator<Item = &str> {
        self.sites.iter().map(|site| site.name.as_str())
    }

    pub fn levels(&self) -> Vec<Bf3Level> {
        self.sites
            .iter()
            .filter(|site| site.base_toc.is_some() || (site.patch_toc.is_some() && !site.delta))
            .filter_map(|site| {
                let id = multiplayer_level_id(&site.name)?;
                Some(Bf3Level {
                    id: id.to_owned(),
                    superbundle: site.name.clone(),
                    expansion: expansion_of(id),
                    base_toc: site.base_toc.clone(),
                    patch_toc: site.patch_toc.clone(),
                })
            })
            .collect()
    }

    pub fn level(&self, level_id: &str) -> Option<Bf3Level> {
        self.levels()
            .into_iter()
            .find(|level| level.id.eq_ignore_ascii_case(level_id))
    }

    pub fn mount_level(self: &Arc<Self>, level_id: &str) -> Result<LevelMount, Bf3Error> {
        let level = self
            .level(level_id)
            .ok_or_else(|| Bf3Error::UnknownLevel(level_id.to_owned()))?;
        LevelMount::mount(Arc::clone(self), level)
    }

    /// Probe addition: mount any superbundle (not only MP levels) through the same machinery.
    pub fn mount_superbundle(self: &Arc<Self>, name: &str) -> Result<LevelMount, Bf3Error> {
        let site = self
            .site(name)
            .ok_or_else(|| Bf3Error::UnknownLevel(name.to_owned()))?;
        let id = name.rsplit('/').next().unwrap_or(name).to_owned();
        let level = Bf3Level {
            expansion: expansion_of(&id),
            id,
            superbundle: site.name.clone(),
            base_toc: site.base_toc.clone(),
            patch_toc: site.patch_toc.clone(),
        };
        LevelMount::mount(Arc::clone(self), level)
    }

    pub(crate) fn site(&self, name: &str) -> Option<&SuperbundleSite> {
        self.sites
            .iter()
            .find(|site| site.name.eq_ignore_ascii_case(name))
    }

    pub(crate) fn files(&self) -> &FileCache {
        &self.files
    }
}

pub(crate) fn read_toc(path: &Path) -> Result<Toc, Bf3Error> {
    deobfuscate(read_whole(path)?)
        .and_then(|plain| parse_toc(&plain))
        .map_err(|error| Bf3Error::format(path.display(), error))
}

pub(crate) fn superbundle_file(toc: &Path) -> PathBuf {
    let exact = toc.with_extension("sb");
    if exact.exists() {
        return exact;
    }
    let (Some(dir), Some(stem)) = (toc.parent(), toc.file_stem()) else {
        return exact;
    };
    let wanted = format!("{}.sb", stem.to_string_lossy());
    resolve(dir, &wanted).unwrap_or(exact)
}

pub(crate) fn mounted_packages(root: &Path) -> (Vec<Bf3Package>, Option<Bf3Package>) {
    let mut packages = discover_packages(root);
    let patch = packages
        .iter()
        .position(|package| package.authoritative)
        .map(|at| packages.remove(at));
    let patch_version = patch.as_ref().and_then(|patch| patch.version);
    packages.retain(|package| match (package.require_version, patch_version) {
        (Some(required), Some(have)) => required <= have,
        (Some(_), None) => false,
        (None, _) => true,
    });
    packages.sort_by(|a, b| (a.mount_order, &a.name).cmp(&(b.mount_order, &b.name)));
    (packages, patch)
}

fn discover_packages(root: &Path) -> Vec<Bf3Package> {
    let Some(update) = resolve(root, "Update") else {
        return Vec::new();
    };
    let Ok(entries) = std::fs::read_dir(&update) else {
        return Vec::new();
    };
    let mut packages = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|dir| dir.is_dir())
        .filter_map(|dir| {
            let text = std::fs::read_to_string(resolve(&dir, "package.mft")?).ok()?;
            let manifest = parse_package_manifest(&text);
            let folder = dir.file_name()?.to_string_lossy().into_owned();
            Some(Bf3Package {
                name: manifest.name.unwrap_or(folder),
                data_dir: resolve(&dir, "Data").unwrap_or_else(|| dir.join("Data")),
                authoritative: manifest.authoritative,
                version: manifest.version,
                require_version: manifest.require_version,
                mount_order: manifest.mount_order.unwrap_or(0),
            })
        })
        .collect::<Vec<_>>();
    packages.sort_by(|a, b| a.name.cmp(&b.name));
    packages
}

fn multiplayer_level_id(superbundle: &str) -> Option<&str> {
    let mut parts = superbundle.split('/');
    let (Some(platform), Some(levels), Some(folder), Some(id), None) = (
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
    ) else {
        return None;
    };
    let multiplayer = id
        .get(..3)
        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("mp_"))
        || expansion_of(id).is_some();
    (platform.eq_ignore_ascii_case("win32")
        && levels.eq_ignore_ascii_case("levels")
        && folder.eq_ignore_ascii_case(id)
        && multiplayer)
        .then_some(id)
}

fn expansion_of(level_id: &str) -> Option<u8> {
    let bytes = level_id.as_bytes();
    match bytes {
        [x, p, digit, b'_', ..]
            if x.eq_ignore_ascii_case(&b'x')
                && p.eq_ignore_ascii_case(&b'p')
                && digit.is_ascii_digit() =>
        {
            Some(digit - b'0')
        }
        _ => None,
    }
}
