# Windows native partial-UV coverage validation, 2026-10-12

Source candidate: production bake code at dac5c62de5d9395bbfb726a94ca9330ad4f4178d,
packaged in branch checkpoint 9bfaf3041655bb1adf652365df292977e3e0861c.
The reports identify Blender 5.2.1 LTS, but do not include a binary hash or source
Git SHA. Candidate association uses the supplied matching patch and test workflow.

The user supplied native-coverage-1.json and native-coverage-0.json. Comparison
confirms five matching cases with finite pixel values, alpha 1 for UV coverage
ON and alpha 0 for OFF. The test uses the same 4x4 setting, Margin 0, a fresh
background scene and a quad UV island that covers a sub-sample but no texel center.

| Case | Low Resolution Mesh | Alpha ON | Alpha OFF |
| --- | --- | --- | --- |
| Normal (tangent-space baker) | ON | 1 | 0 |
| Vector Object | ON | 1 | 0 |
| Vector Tangent | ON | 1 | 0 |
| Vector Object | OFF | 1 | 0 |
| Vector Tangent | OFF | 1 | 0 |

The Normal report's space=OBJECT is the displacement_space setting, which is
ignored by the Normal baker; it is not an object-space Normal bake validation.

Normal ON is (0.5, 0.5, 1, 1). Vector Low Resolution Mesh ON has nonzero X/Y
object offsets of approximately 0.0694444 and the expected tangent channel
ordering. Low Resolution Mesh OFF produces offsets around 1e-8 on this
unsculpted plane, consistent with numerical near-zero residuals. These values
are coverage evidence, not an accuracy benchmark on a sculpted surface.

The uploaded multires-bake-uv-coverage.patch is byte-identical to the kit patch.
SHA256: 0e623a235da9259c7db3701cd47c90840144f86bdb27bc6caa2d435b8f5c330d.

Validated scope: the specific center-only UV culling regression is fixed in this
native smoke test, including both Vector spaces and both low-reference paths.
This also confirms the native script/operator API executes in the user's build.

Not validated by these reports: scalar baking, custom/split normals, complete
seam handling, stacked-island policy, UDIM padding, float-image reproducibility
across native worker counts, large-image time/memory, or bake-to-render geometric
accuracy. The current orthonormal HQ TBN remains unchanged and under investigation.

The paired original reports are retained here as regression evidence. Main is
unchanged. Preserve a source checkpoint before continuing with another bake fix.
