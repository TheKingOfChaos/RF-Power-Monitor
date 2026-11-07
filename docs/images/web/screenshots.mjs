// Screenshots of the device's web page for docs/images/web. Run with screenshots.ps1 (Playwright
// Docker image). The web password comes from RFPOWER_WEB_PASSWORD and is never printed.
//
// The docs are public: every real value is replaced before the picture is taken, in text and in
// form fields: IPv4 addresses become 192.0.2.x, IPv6 prefixes 2001:db8:0:1, the MAC 02:00:00:12:34:56
// (and its tail in the hostname and the EUI-64 interface ids), the contact ops@example.com, the
// location "Rack 4" and the community "public".
import { chromium } from 'playwright';

const ip = process.argv[2];
const out = '/out';
const password = process.env.RFPOWER_WEB_PASSWORD || '';
if (!ip || !password) {
  console.log('usage: node screenshots.mjs <device ip> (RFPOWER_WEB_PASSWORD set)');
  process.exit(2);
}
const base = `http://${ip}`;

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });

async function anonymize() {
  await page.evaluate(() => {
    const macText = document.body.innerText.match(/\b([0-9A-F]{2}(?::[0-9A-F]{2}){5})\b/);
    const mac = macText ? macText[1].toLowerCase().split(':') : null;
    const tail = mac ? mac.slice(3).join('') : null;  // RF-Power-<tail>
    // EUI-64 interface id from the MAC: first byte with the U/L bit flipped, ff:fe in the middle
    const eui = mac
      ? [(parseInt(mac[0], 16) ^ 2).toString(16) + mac[1], mac[2] + 'ff', 'fe' + mac[3], mac[4] + mac[5]]
          .map((h) => h.replace(/^0+(?=.)/, ''))
          .join(':')
      : null;
    const fix = (s) => {
      // The MAC looks like an IPv6 prefix to the rule below: park it until the addresses are done
      if (mac) s = s.replace(new RegExp(mac.join(':'), 'gi'), '@MAC@');
      if (eui) s = s.replace(new RegExp(eui, 'gi'), '0:ff:fe12:3456');
      if (tail) s = s.replace(new RegExp(tail, 'gi'), '123456');
      s = s.replace(/\b(?:10|172|192\.168)\.\d{1,3}\.\d{1,3}\.(\d{1,3})\b/g, '192.0.2.$1');
      s = s.replace(/\b(?!fe80)[0-9a-f]{1,4}:[0-9a-f]{1,4}:[0-9a-f]{1,4}:[0-9a-f]{1,4}:(?=[0-9a-f:])/gi, '2001:db8:0:1:');
      s = s.replace(/fe80::0:/gi, 'fe80::');
      s = s.replace(/[\w.+-]+@[\w-]+(\.[\w-]+)+/g, 'ops@example.com');
      return s.replace(/@MAC@/g, '02:00:00:12:34:56');
    };
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) n.nodeValue = fix(n.nodeValue);
    for (const input of document.querySelectorAll('input')) {
      if (input.name === 'community') input.value = 'public';
      else if (input.name === 'location') input.value = 'Rack 4';
      else input.value = fix(input.value);
      input.setAttribute('value', input.value);
    }
    // Location in the status table, if shown
    for (const td of document.querySelectorAll('td')) {
      if (td.previousElementSibling && /Location/.test(td.previousElementSibling.textContent)) td.textContent = 'Rack 4';
    }
  });
}

await page.goto(`${base}/`);
await anonymize();
await page.screenshot({ path: `${out}/web-login.png` });
console.log('web-login.png');

await page.fill('input[name="password"]', password);
await Promise.all([page.waitForLoadState('networkidle'), page.click('button[type="submit"]')]);
if (!(await page.locator('text=Log out').count())) {
  console.log('login failed');
  process.exit(1);
}
await anonymize();
await page.screenshot({ path: `${out}/web-status.png`, fullPage: true });
console.log('web-status.png');

await page.click('button:has-text("Log out")').catch(() => {});
await browser.close();
