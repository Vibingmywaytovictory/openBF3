use crate::dbobject::parse_db_object;
use crate::error::Error;
use crate::record::Record;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Layout {
    pub superbundles: Vec<SuperbundleDecl>,
    pub filesystems: Vec<String>,
    pub head: Option<i64>,
    pub base: Option<i64>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SuperbundleDecl {
    pub name: String,
    pub delta: bool,
}

pub fn parse_layout(plain: &[u8]) -> Result<Layout, Error> {
    let root = parse_db_object(plain)?;
    let layout = Record::new("layout", &root)?;
    let mut superbundles = Vec::new();
    for entry in layout.list("superBundles")? {
        let entry = Record::new("layout superbundle", entry)?;
        superbundles.push(SuperbundleDecl {
            name: entry.str("name")?.to_owned(),
            delta: entry.flag("delta")?,
        });
    }
    let filesystems = layout
        .list("fs")?
        .iter()
        .map(|value| {
            value.as_str().map(str::to_owned).ok_or(Error::FieldType {
                what: "layout",
                field: "fs",
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(Layout {
        superbundles,
        filesystems,
        head: layout.opt_i64("head")?,
        base: layout.opt_i64("base")?,
    })
}
