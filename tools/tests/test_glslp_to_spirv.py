import glob
import os
import shutil
from pathlib import Path

import pytest

from tools.glslp_to_spirv import (Param, Preset, PresetError, Uniform, c_header, compile_preset, main, parse_params,
                                  read_preset, to_vulkan)

ROOT = Path(__file__).resolve().parents[2]
BUNDLED = sorted(glob.glob(str(ROOT / "snesrecomp/runner/assets/shaders/*.glslp"))) + [
    str(ROOT / "runner/assets/shaders/pvm.glslp")]

PASS = """#version 450
#pragma parameter BEAM "Beam width" 0.5 0.1 1.0 0.05
uniform sampler2D Texture;
uniform vec2 InputSize;
uniform float BEAM;
uniform vec2 OutputSize;
in vec2 vTexCoord;
out vec4 FragColor;
void main() { FragColor = texture(Texture, vTexCoord) * BEAM; }
"""


@pytest.fixture
def glslc() -> str:
    ndk = [os.environ.get("ANDROID_NDK", ""), os.environ.get("ANDROID_HOME", "") + "/ndk/*",
           os.path.expanduser("~/Android/Sdk/ndk/*")]
    found = shutil.which("glslc") or next(
        iter(sorted(p for d in ndk if d for p in glob.glob(d + "/shader-tools/*/glslc"))), None)
    if not found:
        pytest.skip("no glslc on the PATH or in the NDK")
    return found


def _preset(tmp_path: Path, keys: str) -> Path:
    path = tmp_path / "x.glslp"
    path.write_text(keys, encoding="utf-8")
    return path


def test_std140_offsets_follow_declaration_order():
    preset = Preset("x", False, Path("x.glsl"))
    text = to_vulkan(PASS, preset)
    assert [(u.name, u.offset) for u in preset.uniforms] == [("InputSize", 0), ("BEAM", 8), ("OutputSize", 16)]
    assert preset.size == 32
    assert "layout(set = 2, binding = 0) uniform sampler2D Texture;" in text
    assert "layout(set = 3, binding = 0, std140) uniform TlkUniforms" in text
    assert "layout(location = 1) in vec2 vTexCoord;" in text


def test_vec3_aligns_to_sixteen():
    preset = Preset("x", False, Path("x.glsl"))
    to_vulkan("uniform float A;\nuniform vec3 B;\nuniform float C;\nin vec2 vTexCoord;\n", preset)
    assert [(u.name, u.offset) for u in preset.uniforms] == [("A", 0), ("B", 16), ("C", 28)]
    assert preset.size == 32


def test_vec4_aligns_to_sixteen():
    preset = Preset("x", False, Path("x.glsl"))
    to_vulkan("uniform float A;\nuniform vec4 B;\nin vec2 vTexCoord;\n", preset)
    assert [(u.name, u.offset) for u in preset.uniforms] == [("A", 0), ("B", 16)]


def test_pragma_parameter_parsed():
    (p,) = parse_params(PASS)
    assert (p.name, p.label, p.default, p.min, p.max, p.step) == ("BEAM", "Beam width", 0.5, 0.1, 1.0, 0.05)


def test_multipass_preset_refused(tmp_path):
    with pytest.raises(PresetError, match="2 passes"):
        read_preset(_preset(tmp_path, "shaders = 2\nshader0 = a.glsl\nshader1 = b.glsl\n"))


@pytest.mark.parametrize("decl, word", [
    pytest.param("in vec4 vColor;", "vColor", id="another varying"),
    pytest.param("out vec4 Other;", "Other", id="another output"),
    pytest.param("uniform sampler2D LUT;", "LUT", id="a second texture"),
])
def test_unsupported_interface_refused(decl, word):
    with pytest.raises(PresetError, match=word):
        to_vulkan(f"{decl}\nin vec2 vTexCoord;\n", Preset("x", False, Path("x.glsl")))


@pytest.mark.parametrize("keys, linear", [
    pytest.param("filter_linear0 = true\n", True, id="true"),
    pytest.param("filter_linear0 = 1\n", True, id="one"),
    pytest.param("", False, id="absent"),
    pytest.param("# filter_linear0 = true\n", False, id="commented out"),
])
def test_filter_linear_read(tmp_path, keys, linear):
    assert read_preset(_preset(tmp_path, "shaders = 1\nshader0 = a.glsl\n" + keys)).filter_linear == linear


def test_qualified_varying_rewritten():
    text = to_vulkan("smooth in vec2 vTexCoord;\nout vec4 FragColor;\n", Preset("x", False, Path("x.glsl")))
    assert "layout(location = 1) in vec2 vTexCoord;" in text
    assert "smooth" not in text


def test_header_rows_carry_offset_size_and_int_flag():
    preset = Preset("x", False, Path("x.glsl"), [Param("P", 'say "hi"', 1.0, 0.0, 2.0, 0.5)],
                    [Uniform("InputSize", "vec2", 0), Uniform("FrameCount", "int", 8)], 16, b"\x03\x02\x23\x07")
    header = c_header([preset])
    assert '{ "InputSize", 0, 2, 0 }, { "FrameCount", 8, 1, 1 }' in header
    assert '{ "P", "say \\"hi\\"", 1.0f, 0.0f, 2.0f, 0.5f }' in header
    assert '{ "x", 0, kSpirv0, sizeof kSpirv0, kUniforms0, 2, 16, kParams0, 1 },' in header


def test_unchanged_header_not_rewritten(tmp_path):
    out = tmp_path / "h.h"
    assert main(["--out", str(out)]) == 0
    os.utime(out, ns=(1, 1))
    assert main(["--out", str(out)]) == 0
    assert out.stat().st_mtime_ns == 1


def test_no_presets_still_a_header():
    header = c_header([])
    assert "kTlkGpuPresetCount = 0" in header


def test_bundled_presets_compile(glslc):
    presets = [compile_preset(Path(p), glslc) for p in BUNDLED]
    assert [p.name for p in presets] == ["crt-soft", "lcd-grid", "sharp", "warm-composite", "pvm"]
    for p in presets:
        assert p.spirv[:4] == bytes.fromhex("03022307")
    pvm = presets[-1]
    names = ["BEAM_MIN", "BEAM_MAX", "MASK", "MASK_WIDTH", "GLOW", "SHARPNESS", "BRIGHTNESS"]
    assert [q.name for q in pvm.params] == names
    # Uniforms, not #defines, or shader_param would change nothing.
    assert [u.name for u in pvm.uniforms] == ["TextureSize"] + names


def test_glslc_failure_names_the_preset(tmp_path, glslc, capsys):
    (tmp_path / "bad.glsl").write_text("#ifdef FRAGMENT\nvoid main() { nonsense; }\n#endif\n", encoding="utf-8")
    preset = tmp_path / "broken.glslp"
    preset.write_text("shaders = 1\nshader0 = bad.glsl\n", encoding="utf-8")
    assert main(["--glslc", glslc, "--out", str(tmp_path / "h.h"), str(preset)]) == 1
    assert "broken.glslp" in capsys.readouterr().err
