#include "host_ui.h"
#include <cassert>
#include <cstdio>

using stepcontrol::ButtonEvent;
using stepcontrol::HoldButton;

bool black(hostui::Controller& ui, uint32_t now) {
  stepcontrol::Rgb frame[rgbshow::kLedCount];
  ui.render(now,frame);
  for (const auto& p : frame) if (p.r || p.g || p.b) return false;
  return true;
}

int main() {
  HoldButton button;
  hostui::Controller ui;
  ui.reset(0);
  assert(ui.setParameters(50,20,0,1));
  button.begin(false,0);
  const uint8_t initialColor = ui.color(3);
  unsigned holds = 0, clicks = 0;
  auto sample = [&](bool down, uint32_t now) {
    const auto event = button.update(down,now);
    if (event == ButtonEvent::Hold) { ui.toggleRgb(); ++holds; }
    if (event == ButtonEvent::Click) { ui.press(3,now); ++clicks; }
    return event;
  };
  assert(ui.rgbEnabled() && !black(ui,0));
  assert(sample(true,100)==ButtonEvent::None);
  assert(sample(false,105)==ButtonEvent::None); // Bounce cannot create a click.
  assert(sample(true,110)==ButtonEvent::None);
  assert(sample(true,135)==ButtonEvent::None);
  assert(sample(true,2109)==ButtonEvent::None && ui.rgbEnabled());
  assert(sample(true,2110)==ButtonEvent::Hold && !ui.rgbEnabled());
  assert(black(ui,2110));
  assert(sample(true,10000)==ButtonEvent::None && holds==1);
  assert(sample(false,10001)==ButtonEvent::None);
  assert(sample(false,10026)==ButtonEvent::None && clicks==0);
  assert(ui.color(3)==initialColor && ui.value(3)==1);
  // RGB stays off during PID edits, color changes, and the idle transition.
  ui.move(0,1,10100);
  ui.move(3,1,10200);
  ui.press(1,10300);
  assert(ui.value(0)==51 && ui.value(1)==20 && ui.value(3)==10);
  assert(black(ui,10100) && black(ui,10300) && black(ui,20000));
  assert(sample(true,21000)==ButtonEvent::None);
  assert(sample(true,21025)==ButtonEvent::None);
  assert(sample(true,23000)==ButtonEvent::Hold && ui.rgbEnabled());
  assert(!black(ui,23000) && holds==2 && clicks==0);
  sample(false,23001); sample(false,23026);
  sample(true,24000); sample(true,24025);
  sample(false,24100);
  assert(sample(false,24125)==ButtonEvent::Click && clicks==1);
  assert(ui.rgbEnabled() && ui.color(3)==(initialColor+1)%stepcontrol::kPaletteCount);
  assert(ui.value(0)==51 && ui.value(1)==20 && ui.value(2)==0 && ui.value(3)==10);

  // Release just before the deadline must stay a click, including debounce time.
  button.begin(false,0);
  assert(button.update(true,100)==ButtonEvent::None);
  assert(button.update(true,125)==ButtonEvent::None);
  assert(button.update(false,2099)==ButtonEvent::None);
  assert(button.update(false,2124)==ButtonEvent::Click);
  assert(button.update(false,3000)==ButtonEvent::None);
  // If a poll skipped the threshold, a genuinely long release still fires once.
  button.begin(false,0);
  button.update(true,100); button.update(true,125);
  assert(button.update(false,2100)==ButtonEvent::None);
  assert(button.update(false,2125)==ButtonEvent::Hold);
  assert(button.update(false,4000)==ButtonEvent::None);
  // Held-at-boot is ignored until a full release and a new press.
  button.begin(true,0);
  assert(button.update(true,9000)==ButtonEvent::None);
  button.update(false,9010);
  assert(button.update(false,9035)==ButtonEvent::None);
  button.update(true,9100); button.update(true,9125);
  assert(button.update(true,11100)==ButtonEvent::Hold);
  // Millisecond wrap does not shorten, repeat or lose the hold.
  const uint32_t start = UINT32_MAX-100;
  button.begin(false,start-1);
  button.update(true,start); button.update(true,uint32_t(start+25));
  assert(button.update(true,uint32_t(start+1999))==ButtonEvent::None);
  assert(button.update(true,uint32_t(start+2000))==ButtonEvent::Hold);
  assert(button.update(true,uint32_t(start+3000))==ButtonEvent::None);
  button.update(false,uint32_t(start+3001));
  assert(button.update(false,uint32_t(start+3026))==ButtonEvent::None);
  ui.setRgbEnabled(false);
  ui.reset(0);
  assert(ui.rgbEnabled()); // Runtime preference only: power-up defaults to on.
  puts("PASS: STEP 2s hold toggles once, release click stays separate, RGB stays off during edits/idle, values preserved, boot/bounce/deadline/rollover");
}
