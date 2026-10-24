// Haltech CAN Broadcast Protocol V2.35 channel list (big endian, 1 Mbit/s, 11-bit IDs).
// Each entry: id, first bit "byte:bit", last bit "byte:bit", signed, name, kind, unit, convert, enum
//   kind: p = pressure (gauge kPa), pa = pressure (kPa absolute), t = temperature (Kelvin),
//         s = speed (km/h), b = on/off, e = list, n = plain number
// Bit "B:b" = byte B, bit b (bit 7 = most significant). Byte range a-b = "a:7".."b:0".
"use strict";
const H = [];
const R = (id, a, b, sg, name, kind, unit, f) => H.push({ id, from: `${a}:7`, to: `${b}:0`, sg, name, kind, unit, f });
const BIT = (id, pos, name) => H.push({ id, from: pos, to: pos, sg: false, name, kind: "b", unit: "", f: x => x });
const E = (id, from, to, sg, name, map) => H.push({ id, from, to, sg, name, kind: "e", unit: "", f: x => x, map });
const d10 = x => x / 10, d100 = x => x / 100, d1000 = x => x / 1000, same = x => x, gp = x => x / 10 - 101.3;

R(0x360,0,1,0,"RPM","n","rpm",same); R(0x360,2,3,0,"Manifold pressure","pa","",d10);
R(0x360,4,5,0,"Throttle position","n","%",d10); R(0x360,6,7,0,"Coolant pressure","p","",gp);
R(0x361,0,1,0,"Fuel pressure","p","",gp); R(0x361,2,3,0,"Oil pressure","p","",gp);
R(0x361,4,5,0,"Engine demand","n","%",d10); R(0x361,6,7,0,"Wastegate pressure","p","",gp);
R(0x362,0,1,0,"Injection stage 1 duty","n","%",d10); R(0x362,2,3,0,"Injection stage 2 duty","n","%",d10);
R(0x362,4,5,1,"Ignition angle","n","°",d10);
R(0x363,0,1,1,"Wheel slip","s","",d10); R(0x363,2,3,1,"Wheel diff","s","",d10);
for (let i = 0; i < 4; i++) R(0x368,i*2,i*2+1,0,`Wideband sensor ${i+1}`,"n","λ",d1000);
R(0x369,0,1,0,"Trigger error count","n","",same); R(0x369,2,3,0,"Trigger counter","n","",same);
R(0x369,6,7,0,"Trigger sync level","n","",same);
R(0x36A,0,1,0,"Knock level 1","n","dB",d100); R(0x36A,2,3,0,"Knock level 2","n","dB",d100);
R(0x36B,0,1,0,"Brake pressure","p","",x => x - 101.3); R(0x36B,2,3,0,"NOS pressure 1","p","",x => x*11/50 - 101.3);
R(0x36B,4,5,0,"Turbo speed 1","n","rpm",x => x*10); R(0x36B,6,7,1,"Lateral G","n","m/s²",d10);
["front left","front right","rear left","rear right"].forEach((w,i) => R(0x36C,i*2,i*2+1,0,`Wheel speed ${w}`,"s","",d10));
R(0x36D,4,5,1,"Exhaust cam angle 1","n","°",d10); R(0x36D,6,7,1,"Exhaust cam angle 2","n","°",d10);
E(0x36E,"0:7","1:0",0,"Engine limiting active",{0:"Off",1:"On"});
R(0x36E,2,3,1,"Launch control ignition retard","n","°",d10); R(0x36E,4,5,1,"Launch control fuel enrich","n","%",d10);
R(0x36E,6,7,1,"Longitudinal G","n","m/s²",d10);
R(0x36F,0,1,0,"Generic output 1 duty","n","%",d10); R(0x36F,2,3,0,"Boost control output","n","%",d10);
R(0x370,0,1,0,"Vehicle speed (ECU)","s","",d10); R(0x370,4,5,1,"Intake cam angle 1","n","°",d10);
R(0x370,6,7,1,"Intake cam angle 2","n","°",d10);
R(0x371,0,1,0,"Fuel flow","n","cc/min",same); R(0x371,2,3,0,"Fuel flow return","n","cc/min",same);
R(0x372,0,1,0,"Battery voltage","n","V",d10);
R(0x372,4,5,0,"Target boost","p","",d10);            // sent as boost (gauge) pressure, checked on the car
R(0x372,6,7,0,"Barometric pressure","pa","",d10);
for (let i = 0; i < 12; i++) R(0x373 + Math.floor(i/4), (i%4)*2, (i%4)*2+1, 0, `EGT ${i+1}`, "t", "", d10);
R(0x376,0,1,0,"Ambient air temperature","t","",d10); R(0x376,2,3,1,"Relative humidity","n","%",d10);
R(0x376,4,5,0,"Specific humidity","n","ppm",x => x*100); R(0x376,6,7,0,"Absolute humidity","n","g/m³",d10);
R(0x3E0,0,1,0,"Coolant temperature","t","",d10); R(0x3E0,2,3,0,"Air temperature","t","",d10);
R(0x3E0,4,5,0,"Fuel temperature","t","",d10); R(0x3E0,6,7,0,"Oil temperature","t","",d10);
R(0x3E1,0,1,0,"Gearbox oil temperature","t","",d10); R(0x3E1,2,3,0,"Diff oil temperature","t","",d10);
R(0x3E1,4,5,0,"Fuel composition (ethanol)","n","%",d10);
R(0x3E2,0,1,0,"Fuel level","n","L",d10);
R(0x3E3,0,1,1,"Fuel trim short term bank 1","n","%",d10); R(0x3E3,2,3,1,"Fuel trim short term bank 2","n","%",d10);
R(0x3E3,4,5,1,"Fuel trim long term bank 1","n","%",d10); R(0x3E3,6,7,1,"Fuel trim long term bank 2","n","%",d10);
[["1:7","Neutral switch"],["1:6","Reverse switch"],["1:5","Gear switch"],["1:4","Decel cut active"],
 ["1:3","Transient throttle active"],["1:2","Brake pedal switch"],["1:1","Clutch switch"],["1:0","Oil pressure light"],
 ["2:7","Launch control active"],["2:6","Launch control switch"],["2:5","Aux RPM limiter active"],["2:3","Flat shift switch"],
 ["2:1","Torque reduction active"],["3:7","Traction control active"],["3:6","Traction control enabled"],
 ["3:5","Air con request"],["3:4","Air con output"],["3:3","Thermo-fan 4 on"],["3:2","Thermo-fan 3 on"],
 ["3:1","Thermo-fan 2 on"],["3:0","Thermo-fan 1 on"],["7:7","Check engine light"],["7:6","Battery light"]]
 .forEach(([p,n]) => BIT(0x3E4,p,n));
R(0x3E4,4,4,1,"Rotary trim pot 1","n","",same); R(0x3E4,5,5,1,"Rotary trim pot 2","n","",same);
R(0x3E4,6,6,1,"Rotary trim pot 3","n","",same);
E(0x3E5,"0:7","0:0",0,"Ignition switch",{0:"Off",1:"On"});
R(0x3E5,1,1,0,"Turbo timer time remaining","n","s",same); R(0x3E5,2,2,0,"Turbo timer engine time remaining","n","s",same);
R(0x3E5,4,5,1,"Steering wheel angle","n","°",d10); R(0x3E5,6,7,0,"Driveshaft RPM","n","rpm",same);
for (let i = 0; i < 3; i++) R(0x3E6,i*2,i*2+1,0,`NOS pressure ${i+2}`,"p","",x => x*11/50 - 101.3);
R(0x3E6,6,7,0,"Turbo speed 2","n","rpm",x => x*10);
for (let i = 0; i < 8; i++) R(0x3E7 + Math.floor(i/4), (i%4)*2, (i%4)*2+1, 0, `Generic sensor ${i+1}`, "n", "%", d10);
R(0x3E9,0,1,0,"Generic sensor 9","n","%",d10); R(0x3E9,2,3,0,"Generic sensor 10","n","%",d10);
R(0x3E9,4,5,0,"Target lambda","n","λ",d1000);
for (let i = 0; i < 6; i++) BIT(0x3E9, `6:${7-i}`, `Nitrous stage ${i+1} output`);
R(0x3E9,7,7,1,"Torque management knob","n","",same);
R(0x3EA,0,1,0,"Gearbox line pressure","p","",gp); R(0x3EA,2,3,0,"Injection stage 3 duty","n","%",d10);
R(0x3EA,4,5,0,"Injection stage 4 duty","n","%",d10); R(0x3EA,6,7,0,"Crank case pressure","p","",gp);
R(0x3EB,0,3,0,"Race timer","n","ms",same); R(0x3EB,4,5,1,"Ignition angle bank 1","n","°",d10);
R(0x3EB,6,7,1,"Ignition angle bank 2","n","°",d10);
R(0x3EC,0,1,1,"TM driveshaft RPM target","n","rpm",same); R(0x3EC,2,3,1,"TM driveshaft RPM target error","n","rpm",same);
R(0x3EC,4,5,1,"TM target error ignition correction","n","°",d10); R(0x3EC,6,7,1,"TM timed ignition correction","n","°",d10);
R(0x3ED,0,1,1,"TM combined ignition correction","n","°",d10);
for (let i = 0; i < 8; i++) R(0x3EE + Math.floor(i/4), (i%4)*2, (i%4)*2+1, 0, `Wideband sensor ${i+5}`, "n", "λ", d1000);
R(0x470,0,1,0,"Wideband overall","n","λ",d1000); R(0x470,2,3,0,"Wideband bank 1","n","λ",d1000);
R(0x470,4,5,0,"Wideband bank 2","n","λ",d1000);
E(0x470,"6:7","6:0",1,"Gear selector position",{0:"Neutral","-1":"Reverse","-2":"Park","-3":"Unknown","-4":"Drive","-5":"Sport","-6":"Manual","-7":"Low","-8":"Overdrive"});
E(0x470,"7:7","7:0",1,"Gear (ECU)",{"-1":"R",0:"N",1:"1",2:"2",3:"3",4:"4",5:"5",6:"6"});
R(0x471,0,1,1,"Injector pressure differential","n","kPa",d10); R(0x471,2,3,0,"Accelerator pedal position","n","%",d10);
R(0x472,0,1,0,"Cruise target speed","s","",d10); R(0x472,2,3,0,"Cruise last target speed","s","",d10);
R(0x472,4,5,1,"Cruise speed error","s","",d10);
E(0x472,"6:7","6:4",0,"Cruise controller state",{0:"Disabled",1:"Inactive",2:"Cruising",3:"Accelerating",4:"Decelerating"});
H.push({ id: 0x472, from: "6:3", to: "7:0", sg: false, name: "Cruise input state", kind: "n", unit: "", f: x => x });

H.forEach((c, i) => { c.key = `${c.id.toString(16)}_${i}`; });

// Pull a bit field out of an 8-byte big-endian message
function haltechRaw(data, from, to, signed) {
  const pos = s => { const [B, b] = s.split(":").map(Number); return B * 8 + (7 - b); };
  const a = pos(from), z = pos(to), len = z - a + 1;
  let v = 0;
  for (let p = a; p <= z; p++) v = v * 2 + ((data[p >> 3] >> (7 - (p & 7))) & 1);
  if (signed && v >= 2 ** (len - 1)) v -= 2 ** len;
  return v;
}
