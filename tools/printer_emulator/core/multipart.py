"""multipart/form-data, as curl's form_add / form_add_file send it.

Lifted from tests/slic3rutils/fake_moonraker.py's multipart_file(), extended to return the plain
fields as well (Moonraker's `print`, `path`, `root`; OctoPrint's `print`, `select`).
"""
import email.parser
import email.policy


def parse(content_type, body):
    """-> (fields: {name: str}, files: [(field, filename, bytes)])"""
    fields, files = {}, []
    if not content_type or "multipart/" not in content_type.lower():
        return fields, files
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        b"Content-Type: " + content_type.encode("latin-1") + b"\r\n\r\n" + body)
    if not msg.is_multipart():
        return fields, files
    for part in msg.iter_parts():
        name = part.get_param("name", header="content-disposition") or ""
        filename = part.get_filename()
        data = part.get_payload(decode=True) or b""
        if filename is not None:
            files.append((name, filename, data))
        else:
            fields[name] = data.decode("utf-8", "replace")
    return fields, files
