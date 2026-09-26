#pragma once
#include <string>
#include <vector>

// Canonical name table for post-translation glossary canonicalisation
// (see protect.h's apply_glossary). These are proper nouns from GTA San
// Andreas -- vehicle model names and place/zone names -- that must never be
// left mistranslated or mis-cased after a chat message comes back from a
// translation model. The table only needs the canonical spelling: matching
// is case-insensitive, so any casing variant a model produces is recognised
// and rewritten back to exactly the spelling stored here.

namespace i18n::protect {

inline const std::vector<std::string>& glossary_terms() {
    static const std::vector<std::string> terms = {
        // ---- Vehicles: cars ------------------------------------------------
        "Admiral", "Alpha", "Bandito", "Banshee", "BF Injection", "Blade",
        "Blista Compact", "Bobcat", "Bravura", "Broadway", "Buccaneer",
        "Buffalo", "Bullet", "Cadrona", "Camper", "Cheetah", "Club", "Clover",
        "Comet", "Elegy", "Emperor", "Esperanto", "Euros", "Feltzer", "Flash",
        "Fortune", "Glendale", "Greenwood", "Hermes", "Hotdog", "Hotknife",
        "Hustler", "Infernus", "Intruder", "Jester", "Journey", "Kart",
        "Landstalker", "Majestic", "Manana", "Mesa", "Monster", "Moonbeam",
        "Nebula", "Oceanic", "Patriot", "Perennial", "Phoenix", "Picador",
        "Premier", "Previon", "Primo", "Rancher", "Ranger", "Regina",
        "Remington", "Sabre", "Sabre Turbo", "Sadler", "Sandking", "Savanna",
        "Sentinel", "Slamvan", "Solair", "Stallion", "Stratum", "Sultan",
        "Sunrise", "Super GT", "Tahoma", "Tampa", "Turismo", "Uranus",
        "Vincent", "Virgo", "Voodoo", "Walton", "Washington", "Willard",
        "Windsor", "ZR-350",

        // ---- Vehicles: vans, industrial, emergency, military ---------------
        "Ambulance", "Baggage", "Barracks", "Benson", "Boxville", "Bus",
        "Cabbie", "Caddy", "Coach", "Combine Harvester", "Dozer", "Dumper",
        "Enforcer", "FBI Rancher", "Firetruck", "Flatbed", "Forklift",
        "Linerunner", "Mule", "Packer", "Pizzaboy", "Pony", "Rhino",
        "Roadtrain", "Romero", "Rumpo", "Securicar", "Stretch", "Tanker",
        "Taxi", "Tractor", "Trailer", "Trashmaster", "Yankee",

        // ---- Vehicles: motorcycles, bicycles, quads -------------------------
        "BF-400", "BMX", "Faggio", "FCR-900", "Freeway", "NRG-500",
        "PCJ-600", "Quad", "Sanchez", "Wayfarer",

        // ---- Vehicles: aircraft ---------------------------------------------
        "Andromada", "AT-400", "Beagle", "Crop Duster", "Dodo", "Hydra",
        "Nevada", "Rustler", "Shamal", "Skimmer", "Stunt Plane",

        // ---- Vehicles: helicopters -------------------------------------------
        "Cargobob", "Hunter", "Leviathan", "Maverick", "News Chopper",
        "Police Maverick", "Raindance", "Seasparrow", "Sparrow",

        // ---- Vehicles: boats --------------------------------------------------
        "Coastguard", "Dinghy", "Jetmax", "Launch", "Marquis", "Predator",
        "Reefer", "Speeder", "Squalo", "Tropic", "Tug",

        // ---- Vehicles: trains ---------------------------------------------------
        "Freight", "Streak", "Tram",

        // ---- Vehicles: radio-controlled -----------------------------------------
        "Berkley's RC Van", "RC Bandit", "RC Baron", "RC Goblin", "RC Raider",

        // ---- Vehicles: unique / named specials -----------------------------------
        "Bloodring Banger", "Hotring Racer", "Mr. Whoopee", "Vortex",

        // ---- Places: cities -------------------------------------------------------
        "Los Santos", "San Fierro", "Las Venturas",

        // ---- Places: zones, districts, and landmarks -------------------------------
        "Grove Street", "Ganton", "Idlewood", "Jefferson", "El Corona",
        "Willowfield", "Glen Park", "East Los Santos", "Los Flores",
        "Playa del Seville", "Ocean Docks", "Verona Beach", "Marina",
        "Santa Maria Beach", "Vinewood", "Mulholland", "Richman", "Rodeo",
        "Commerce", "Downtown Los Santos", "Market", "Temple",
        "Verdant Bluffs", "Montgomery", "Blueberry", "Dillimore",
        "Palomino Creek", "Red County", "Flint County", "Whetstone",
        "Angel Pine", "Bayside", "Doherty", "Garcia", "Chinatown",
        "Easter Bay", "Palisades", "Paradiso", "Queens", "Hashbury",
        "Foster Valley", "Juniper Hill", "Calton Heights", "Financial",
        "Downtown San Fierro", "The Strip", "Old Venturas Strip",
        "Redsands East", "Redsands West", "Come-A-Lot", "Roca Escalante",
        "Rockshore East", "Rockshore West", "Julius Thruway", "Prickle Pine",
        "Whitewood Estates", "Creek", "Linden Station", "Pilgrim",
        "Randolph Industrial Estate", "Emerald Isle", "Blackfield",
        "K.A.C.C. Military Fuels", "Bone County", "Tierra Robada",
        "Fort Carson", "Las Payasadas", "El Quebrados", "Aldea Malvada",
        "Verdant Meadows", "Restricted Area", "Area 69", "Regular Tom",
        "Fallen Tree", "Hunter Quarry", "Las Barrancas",
        "Los Santos International", "Easter Basin",
    };
    return terms;
}

} // namespace i18n::protect
