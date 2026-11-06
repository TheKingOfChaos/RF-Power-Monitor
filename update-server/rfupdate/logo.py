"""The RF canopy logo, the same as the device's web page (lib/web_ui/include/logo_svg.h;
tests/test_web.py checks the two stay equal). Shown in the top bar, on the login page
and as the favicon."""

COLOR = "#FF7300"
VIEWBOX = "140 42 270 170"
PATH = (
    "M339.4847107,58.6125908c-12.2723389,23.2532082-34.448822,34.4487953-57.2709961,34.4487953c-26.052490"
    "2,0-49.3056793-16.148407-66.5308685-43.0613861h-3.0140076C210.5153046,91.9846115,182.5255432,128.372"
    "2076,147,152.2711639v3.0140076c13.994873-3.0140076,29.0664978-3.6597595,39.8326263,3.6597748c12.9181"
    "061,8.8288727,20.2376556,23.4694977,20.2376556,45.0001831h3.4450226c11.842865-53.8274994,50.3824615-"
    "93.658577,95.8121185-90.2135315c36.6022949,3.2287445,60.0702209,23.2532043,73.2046509,66.7440338h2.1"
    "535339C384.699646,173.5871277,394.3874207,167.128067,403,164.9745483c-26.9129639-21.315979-56.841522"
    "2-60.932312-60.070282-106.3619537H339.4847107z"
)


def svg(width, height, label=None):
    """The logo as inline SVG markup; label makes it an image with alt text, else decorative."""
    role = 'role="img" aria-label="%s"' % label if label else 'aria-hidden="true"'
    return ('<svg width="%d" height="%d" viewBox="%s" %s><path fill="%s" d="%s"/></svg>'
            % (width, height, VIEWBOX, role, COLOR, PATH))


def favicon():
    """The logo as a standalone SVG file."""
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s"><path fill="%s" d="%s"/></svg>'
            % (VIEWBOX, COLOR, PATH)).encode("ascii")
