// Screenshots of the RF-Power-Monitor device pages in LibreNMS for docs/images/librenms.
// Run with screenshots.ps1 (Playwright Docker image). The login is read from /secret/librenms.md
// (line 1 "username: ...", line 2 the password) and never printed.
import { chromium } from 'playwright';
import fs from 'fs';

const base = process.env.LIBRENMS_URL || 'http://host.docker.internal:8070';
const out = '/out';
const device = process.argv[2] || '1';

const lines = fs.readFileSync('/secret/librenms.md', 'utf8').split(/\r?\n/).filter((l) => l.trim() !== '');
const user = lines[0].replace(/^\s*user(name)?\s*:\s*/i, '').trim();
const pass = lines[1].replace(/^\s*pass(word)?\s*:\s*/i, '').trim();

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
await page.goto(`${base}/login`);
await page.fill('input[name="username"]', user);
await page.fill('input[name="password"]', pass);
await Promise.all([page.waitForLoadState('networkidle'), page.click('button[type="submit"], #login')]);
if (page.url().includes('/login')) {
  console.log('login failed');
  process.exit(1);
}

async function shoot(file) {
  await page.waitForLoadState('networkidle');
  await page.waitForTimeout(1500);  // graphs and widgets finish drawing
  // The docs are public: show a placeholder instead of the real contact address
  await page.evaluate(() => {
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
      n.nodeValue = n.nodeValue.replace(/[\w.+-]+@[\w-]+(\.[\w-]+)+/g, 'ops@example.com');
    }
  });
  await page.screenshot({ path: `${out}/${file}` });
  console.log(`${file}: ${page.url()}`);
}

const shots = [
  ['device-overview.png', `${base}/device/${device}`],
  ['device-eventlog.png', `${base}/device/${device}/logs/eventlog`],
  ['device-neighbours.png', `${base}/device/${device}/neighbours`],
];
for (const [file, url] of shots) {
  // The overview is taller: the sensors sit below the processor and memory graphs
  await page.setViewportSize({ width: 1400, height: file === 'device-overview.png' ? 1700 : 900 });
  await page.goto(url);
  await shoot(file);
}
await page.setViewportSize({ width: 1400, height: 900 });

// Inventory: follow the tab's own link
await page.goto(`${base}/device/${device}`);
const inv = await page.locator('a:has-text("Inventory")').first().getAttribute('href');
await page.goto(inv.startsWith('http') ? inv : base + inv);
await shoot('device-inventory.png');

await browser.close();
