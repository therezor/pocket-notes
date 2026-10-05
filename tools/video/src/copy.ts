// Every line of on-screen text, in scene order. Numbers come from the README.
export const COPY = {
  hook: {
    q: "Can AI be useful on a microchip?",
    yes: "Yes.", a: "It just filed this note.",
    sub: "Offline, on a chip with 512 KB of RAM.",
  },
  sort: { title: "It sorts every note for you." },
  chat: {
    title: "Chatbots write one word at a time.",
    ask: "Which list does \"buy oat milk and coffee beans\" go in?",
    generating: "generating", tokens: "tokens",
    caption: "illustration of a chatbot reply",
    reply: "Great question! Based on the content of your note, it looks like it could belong in your Shopping list, because it mentions items that you",
  },
  decide: { a: "THIS ONE", b: "DECIDES." },
  jev: {
    title: "It uses a Jev-style decision model.",
    sub: "It scores every list in one pass.",
    noteLabel: "NEW NOTE",
    note: "buy oat milk and coffee beans",
    pass: "1 PASS",
    foot: "The model, TinyDecide, doesn't generate any text.",
  },
  stats: {
    cards: [
      { big: "10.4M", small: "parameters" },
      { big: "512 KB", small: "of RAM" },
      { big: "4-bit", small: "weights" },
    ],
    from: "41.6 MB", fromLabel: "full precision",
    to: "6.2 MB", toLabel: "in flash",
    sub: "Each note takes about 1.8 s, on both CPU cores.",
  },
  demo: {
    // `at` is the beat the label appears on; the 68% pick lands on beat 66.
    steps: [
      { at: 62, n: "02", label: "A note with a time in it" },
      { at: 66, n: "03", label: "It picks Events at 68%" },
    ],
    badge: "REAL DEVICE, REAL TIME",
    caption: "The Cardputer sent these frames over USB",
  },
  learn: {
    title: "It learns from you.",
    from: 62, to: 75,
    unit: "right on the first guess",
    fromLabel: "at first",
    toLabel: "after 5 notes per list",
    sub: "Every note you file becomes an example. Nothing leaves the device.",
  },
  no: ["It runs offline.", "It needs no phone or account."],
  logo: {
    a: "POCKET", b: "NOTES",
    tagline: "It files your notes for you.",
    chips: ["Plain Markdown files", "Opens in Obsidian", "Open source, MIT"],
    url: "github.com/therezor/pocket-notes",
    foot: "Free firmware for the M5Stack Cardputer",
  },
};

// The demo's seeded notes and the two filmed ones, by list (in the device's list order).
export const LISTS: { name: string; notes: string[] }[] = [
  { name: "Todo", notes: ["fix the squeaky door hinge", "send Anna the trip photos", "call the plumber about the leak"] },
  { name: "Shopping", notes: ["bananas, bread, eggs", "AA batteries x4", "buy oat milk and coffee beans"] },
  { name: "Ideas", notes: ["solar moisture sensor", "maze game with a tilt sensor"] },
  { name: "Events", notes: ["team dinner fri 7:30pm", "flight to Lisbon june 3", "dentist thursday 4pm"] },
  { name: "Contacts", notes: ["Marta plumber 0161 496 0012", "Jake jake.m@mail.com"] },
  { name: "Notes", notes: ["parked on level 3, row F", "cabin wifi password on the fridge"] },
];

// What the device showed for the filmed note (README demo, step 1).
export const BARS = [
  { name: "Shopping", p: 88 },
  { name: "Ideas", p: 8 },
  { name: "Events", p: 0 },
  { name: "Contacts", p: 0 },
];
