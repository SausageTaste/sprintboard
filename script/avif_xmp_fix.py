import subprocess
import os
import tempfile
import re

try:
    from lxml import etree
except ImportError:
    import xml.etree.ElementTree as etree


# Make a safe XML local name suffix from an arbitrary key
# - keep [A-Za-z0-9_.-]
# - everything else becomes '_'
# - if it starts with a non-letter/_ then prefix with '_'
def sanitize_key(key: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_.-]", "_", key)
    if not s or not re.match(r"[A-Za-z_]", s[0]):
        s = "_" + s
    return s


# CDATA cannot contain "]]>", so split it safely
def cdata(value: str) -> str:
    # Split "]]>" into "]]]]><![CDATA[>"
    return "<![CDATA[" + value.replace("]]>", "]]]]><![CDATA[>") + "]]>"


def build_xmp_from_png_text(kv: dict[str, str]) -> str:
    lines = []
    lines.append('<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>')
    lines.append('<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="sprintboard">')
    lines.append('  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">')
    lines.append('    <rdf:Description rdf:about="" xmlns:sprintboard="https://github.com/SausageTaste/sprintboard/">')

    # stable order (nice for git diffs)
    for key in sorted(kv.keys()):
        local = f"pngText_{sanitize_key(key)}"
        val = kv[key] if kv[key] is not None else ""
        lines.append(f"      <sprintboard:{local}>{cdata(val)}</sprintboard:{local}>")

    lines.append("    </rdf:Description>")
    lines.append("  </rdf:RDF>")
    lines.append("</x:xmpmeta>")
    lines.append("<?xpacket end=\"w\"?>")
    return "\n".join(lines)


def main():
    root_dir = r'C:\Users\KCEI\Documents\GitHub\sprintboard-ui\test_cpp\images'

    for item in os.listdir(root_dir):
        if not item.lower().endswith('.avif'):
            continue

        file_path = os.path.join(root_dir, item)
        print(f'Processing file: {file_path}')

        result = subprocess.run(
            [
                'exiftool',
                '-b', "-XMP",
                file_path,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        xml_str = result.stdout.decode('utf-8')
        xml_data = etree.fromstring(xml_str)
        root = xml_data

        # Find rdf:Description
        desc = root.find(".//{http://www.w3.org/1999/02/22-rdf-syntax-ns#}Description")
        if desc is None:
            raise RuntimeError("No rdf:Description found")

        # Your custom namespace URI (exactly as in the file)
        REFIMG_URI = desc.nsmap.get("refimg")  # -> "refimg/"
        if not REFIMG_URI:
            raise RuntimeError("refimg namespace not declared")

        # Collect all refimg attributes that start with "png."
        png_kv = {}
        for attr_name, value in desc.attrib.items():
            # attr_name looks like "{refimg/}png.prompt"
            if attr_name.startswith(f"{{{REFIMG_URI}}}png."):
                key = attr_name.split("}", 1)[1]   # "png.prompt"
                key = key.removeprefix("png.")
                png_kv[key] = value

        new_xmp = build_xmp_from_png_text(png_kv)

        with tempfile.NamedTemporaryFile(delete=False, suffix=".xmp") as tf:
            tf.write(new_xmp.encode('utf-8'))
            tmp = tf.name

        try:
            result = subprocess.run(
                [
                    "exiftool",
                    "-overwrite_original",
                    f"-XMP<={tmp}",
                    file_path,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            print(result.stdout.strip())
            if result.stderr.strip():
                print("stderr:", result.stderr.strip())

            if "0 image files updated" in result.stdout:
                raise RuntimeError("ExifTool did not update the file")
        finally:
            os.unlink(tmp)

        break


if __name__ == '__main__':
    main()
