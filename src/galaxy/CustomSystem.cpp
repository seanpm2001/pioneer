// Copyright © 2008-2023 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "CustomSystem.h"

#include "Galaxy.h"
#include "SystemPath.h"
#include "core/LZ4Format.h"

#include "../gameconsts.h"
#include "EnumStrings.h"
#include "JsonUtils.h"
#include "Factions.h"
#include "FileSystem.h"
#include "Polit.h"
#include "core/Log.h"
#include "galaxy/SystemBody.h"
#include "lua/LuaConstants.h"
#include "lua/LuaFixed.h"
#include "lua/LuaUtils.h"
#include "lua/LuaVector.h"

#include <map>

const CustomSystemsDatabase::SystemList CustomSystemsDatabase::s_emptySystemList; // see: Null Object pattern

static CustomSystemsDatabase *s_activeCustomSystemsDatabase = nullptr;

// ------- CustomSystemBody --------

static const char LuaCustomSystemBody_TypeName[] = "CustomSystemBody";

static CustomSystemBody **l_csb_check_ptr(lua_State *L, int idx)
{
	CustomSystemBody **csbptr = static_cast<CustomSystemBody **>(
		luaL_checkudata(L, idx, LuaCustomSystemBody_TypeName));
	if (!(*csbptr)) {
		abort();
		luaL_argerror(L, idx, "invalid body (this body has already been used)");
	}
	return csbptr;
}

static CustomSystemBody *l_csb_check(lua_State *L, int idx)
{
	return *l_csb_check_ptr(L, idx);
}

static int l_csb_new(lua_State *L)
{
	const char *name = luaL_checkstring(L, 2);
	int type = LuaConstants::GetConstantFromArg(L, "BodyType", 3);

	if (type < SystemBody::TYPE_GRAVPOINT || type > SystemBody::TYPE_MAX) {
		return luaL_error(L, "body '%s' does not have a valid type", name);
	}

	CustomSystemBody **csbptr = static_cast<CustomSystemBody **>(
		lua_newuserdata(L, sizeof(CustomSystemBody *)));
	*csbptr = new CustomSystemBody;
	luaL_setmetatable(L, LuaCustomSystemBody_TypeName);

	(*csbptr)->name = name;
	(*csbptr)->type = static_cast<SystemBody::BodyType>(type);

	return 1;
}

static double *getDoubleOrFixed(lua_State *L, int which)
{
	static double mixvalue;

	const char *s = luaL_typename(L, which);
	if (strcmp(s, "userdata") == 0) {
		const fixed *mix = LuaFixed::CheckFromLua(L, which);
		mixvalue = mix->ToDouble();
		return (&mixvalue);
	} else if (strcmp(s, "number") == 0) {
		mixvalue = luaL_checknumber(L, which);
		return (&mixvalue);
	}
	return nullptr;
}

// Used when the value MUST not be NEGATIVE but can be Zero, for life, etc
#define CSB_FIELD_SETTER_FIXED(luaname, fieldname)                                                                                        \
	static int l_csb_##luaname(lua_State *L)                                                                                              \
	{                                                                                                                                     \
		CustomSystemBody *csb = l_csb_check(L, 1);                                                                                        \
		double *value = getDoubleOrFixed(L, 2);                                                                                           \
		if (value == nullptr)                                                                                                             \
			return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));                                   \
		if (*value < 0.0)                                                                                                                 \
			Output("Error: Custom system definition: Value cannot be negative (%lf) for %s : %s\n", *value, csb->name.c_str(), #luaname); \
		csb->fieldname = fixed::FromDouble(*value);                                                                                       \
		lua_settop(L, 1);                                                                                                                 \
		return 1;                                                                                                                         \
	}

#define CSB_FIELD_SETTER_REAL(luaname, fieldname)  \
	static int l_csb_##luaname(lua_State *L)       \
	{                                              \
		CustomSystemBody *csb = l_csb_check(L, 1); \
		double value = luaL_checknumber(L, 2);     \
		csb->fieldname = value;                    \
		lua_settop(L, 1);                          \
		return 1;                                  \
	}

#define CSB_FIELD_SETTER_INT(luaname, fieldname)   \
	static int l_csb_##luaname(lua_State *L)       \
	{                                              \
		CustomSystemBody *csb = l_csb_check(L, 1); \
		int value = luaL_checkinteger(L, 2);       \
		csb->fieldname = value;                    \
		lua_settop(L, 1);                          \
		return 1;                                  \
	}

#define CSB_FIELD_SETTER_STRING(luaname, fieldname) \
	static int l_csb_##luaname(lua_State *L)        \
	{                                               \
		CustomSystemBody *csb = l_csb_check(L, 1);  \
		std::string value = luaL_checkstring(L, 2); \
		csb->fieldname = value;                     \
		lua_settop(L, 1);                           \
		return 1;                                   \
	}

CSB_FIELD_SETTER_FIXED(radius, radius)
CSB_FIELD_SETTER_FIXED(mass, mass)
CSB_FIELD_SETTER_INT(temp, averageTemp)
CSB_FIELD_SETTER_FIXED(semi_major_axis, semiMajorAxis)
CSB_FIELD_SETTER_FIXED(eccentricity, eccentricity)
CSB_FIELD_SETTER_REAL(latitude, latitude)
CSB_FIELD_SETTER_REAL(longitude, longitude)
CSB_FIELD_SETTER_FIXED(rotation_period, rotationPeriod)
CSB_FIELD_SETTER_FIXED(axial_tilt, axialTilt)
CSB_FIELD_SETTER_FIXED(metallicity, metallicity)
CSB_FIELD_SETTER_FIXED(volcanicity, volcanicity)
CSB_FIELD_SETTER_FIXED(atmos_density, volatileGas)
CSB_FIELD_SETTER_FIXED(atmos_oxidizing, atmosOxidizing)
CSB_FIELD_SETTER_FIXED(ocean_cover, volatileLiquid)
CSB_FIELD_SETTER_FIXED(ice_cover, volatileIces)
CSB_FIELD_SETTER_FIXED(life, life)
CSB_FIELD_SETTER_STRING(space_station_type, spaceStationType)

#undef CSB_FIELD_SETTER_FIXED
#undef CSB_FIELD_SETTER_FLOAT
#undef CSB_FIELD_SETTER_INT

static int l_csb_radius_km(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	double value = luaL_checknumber(L, 2);
	// earth mean radiusMean radius = 6371.0 km (source: wikipedia)
	csb->radius = (value / 6371.0);
	lua_settop(L, 1);
	return 1;
}

static int l_csb_seed(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	csb->seed = luaL_checkunsigned(L, 2);
	csb->want_rand_seed = false;
	lua_settop(L, 1);
	return 1;
}

static int l_csb_orbital_offset(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	double *value = getDoubleOrFixed(L, 2);
	if (value == nullptr)
		return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
	csb->orbitalOffset = fixed::FromDouble(*value);
	csb->want_rand_offset = false;
	lua_settop(L, 1);
	return 1;
}

static int l_csb_orbital_phase_at_start(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	double *value = getDoubleOrFixed(L, 2);
	if (value == nullptr)
		return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
	if ((*value < 0.0) || (*value > double(2.0 * M_PI)))
		return luaL_error(L, "Error: Custom system definition: Orbital phase at game start must be between 0 and 2 PI radians (including 0 but not 2 PI).");
	csb->orbitalPhaseAtStart = fixed::FromDouble(*value);
	csb->want_rand_phase = false;
	lua_settop(L, 1);
	return 1;
}

static int l_csb_rotational_phase_at_start(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	double *value = getDoubleOrFixed(L, 2);
	if (value == nullptr)
		return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
	if ((*value < 0.0) || (*value > double(2.0 * M_PI)))
		return luaL_error(L, "Error: Custom system definition: Rotational phase at start must be between 0 and 2 PI radians (including 0 but not 2 PI).\n The rotational phase is the phase of the body's spin about it's axis at game start.");
	csb->rotationalPhaseAtStart = fixed::FromDouble(*value);
	lua_settop(L, 1);
	return 1;
}

static int l_csb_height_map(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	const char *fname = luaL_checkstring(L, 2);
	int fractal = luaL_checkinteger(L, 3);
	if (fractal >= 2) {
		return luaL_error(L, "invalid terrain fractal type");
	}

	csb->heightMapFilename = FileSystem::JoinPathBelow("heightmaps", fname);
	csb->heightMapFractal = fractal;
	lua_settop(L, 1);
	return 1;
}

static int l_csb_rings(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	if (lua_isboolean(L, 2)) {
		if (lua_toboolean(L, 2)) {
			csb->ringStatus = CustomSystemBody::WANT_RINGS;
		} else {
			csb->ringStatus = CustomSystemBody::WANT_NO_RINGS;
		}
	} else {
		csb->ringStatus = CustomSystemBody::WANT_CUSTOM_RINGS;
		double *value = getDoubleOrFixed(L, 2);
		if (value == nullptr)
			return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
		csb->ringInnerRadius = fixed::FromDouble(*value);
		value = getDoubleOrFixed(L, 3);
		if (value == nullptr)
			return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 3));
		csb->ringOuterRadius = fixed::FromDouble(*value);
		luaL_checktype(L, 4, LUA_TTABLE);
		Color4f col;
		lua_rawgeti(L, 4, 1);
		col.r = luaL_checknumber(L, -1);
		lua_rawgeti(L, 4, 2);
		col.g = luaL_checknumber(L, -1);
		lua_rawgeti(L, 4, 3);
		col.b = luaL_checknumber(L, -1);
		lua_rawgeti(L, 4, 4);
		col.a = luaL_optnumber(L, -1, 0.85); // default alpha value
		csb->ringColor = col;
	}
	lua_settop(L, 1);
	return 1;
}

static int l_csb_gc(lua_State *L)
{
	CustomSystemBody **csbptr = static_cast<CustomSystemBody **>(
		luaL_checkudata(L, 1, LuaCustomSystemBody_TypeName));
	delete *csbptr; // does nothing if *csbptr is null
	*csbptr = 0;
	return 0;
}

static int l_csb_aspect_ratio(lua_State *L)
{
	CustomSystemBody *csb = l_csb_check(L, 1);
	double *value = getDoubleOrFixed(L, 2);
	if (value == nullptr)
		return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
	csb->aspectRatio = fixed::FromDouble(*value);
	if (csb->aspectRatio < fixed(1, 1)) {
		return luaL_error(
			L, "Error: Custom system definition: Equatorial to Polar radius ratio cannot be less than 1.");
	}
	if (csb->aspectRatio > fixed(10000, 1)) {
		return luaL_error(
			L, "Error: Custom system definition: Equatorial to Polar radius ratio cannot be greater than 10000.0.");
	}
	lua_settop(L, 1);
	return 1;
}

static luaL_Reg LuaCustomSystemBody_meta[] = {
	{ "new", &l_csb_new },
	{ "seed", &l_csb_seed },
	{ "radius", &l_csb_radius },
	{ "radius_km", &l_csb_radius_km },
	{ "equatorial_to_polar_radius", &l_csb_aspect_ratio },
	{ "mass", &l_csb_mass },
	{ "temp", &l_csb_temp },
	{ "semi_major_axis", &l_csb_semi_major_axis },
	{ "eccentricity", &l_csb_eccentricity },
	{ "orbital_offset", &l_csb_orbital_offset },
	{ "orbital_phase_at_start", &l_csb_orbital_phase_at_start },
	{ "latitude", &l_csb_latitude },
	// latitude is for surface bodies, inclination is for orbiting bodies (but they're the same field)
	{ "inclination", &l_csb_latitude },
	{ "longitude", &l_csb_longitude },
	{ "rotation_period", &l_csb_rotation_period },
	{ "rotational_phase_at_start", &l_csb_rotational_phase_at_start }, // 0 to 2 pi
	{ "axial_tilt", &l_csb_axial_tilt },
	{ "height_map", &l_csb_height_map },
	{ "metallicity", &l_csb_metallicity },
	{ "volcanicity", &l_csb_volcanicity },
	{ "atmos_density", &l_csb_atmos_density },
	{ "atmos_oxidizing", &l_csb_atmos_oxidizing },
	{ "ocean_cover", &l_csb_ocean_cover },
	{ "ice_cover", &l_csb_ice_cover },
	{ "space_station_type", &l_csb_space_station_type },
	{ "life", &l_csb_life },
	{ "rings", &l_csb_rings },
	{ "__gc", &l_csb_gc },
	{ 0, 0 }
};

void CustomSystemBody::LoadFromJson(const Json &obj)
{
	// XXX: this is copied from SystemBody::LoadFromJson because this architecture is a bit of a mess

	seed = obj.value<uint32_t>("seed", 0);
	name = obj.value<std::string>("name", "");

	int typeVal = EnumStrings::GetValue("BodyType", obj.value<std::string>("type", "GRAVPOINT").c_str());
	type = SystemBody::BodyType(typeVal);

	radius = obj.value<fixed>("radius", 0);
	aspectRatio = obj.value<fixed>("aspectRatio", 0);
	mass = obj.value<fixed>("mass", 0);
	rotationPeriod = obj.value<fixed>("rotationPeriod", 0);
	// humanActivity = obj.value<fixed>("humanActivity", 0);
	semiMajorAxis = obj.value<fixed>("semiMajorAxis", 0);
	eccentricity = obj.value<fixed>("eccentricity", 0);
	orbitalOffset = obj.value<fixed>("orbitalOffset", 0);
	orbitalPhaseAtStart = obj.value<fixed>("orbitalPhase", 0);
	axialTilt = obj.value<fixed>("axialTilt", 0);
	latitude = obj.value<fixed>("inclination", 0).ToDouble();
	argOfPeriapsis = obj.value<fixed>("argOfPeriapsis", 0);
	averageTemp = obj.value<uint32_t>("averageTemp", 0);
	// isCustomBody = obj.value<bool>("isCustom", false);

	metallicity = obj.value<fixed>("metallicity", 0);
	volatileGas = obj.value<fixed>("volatileGas", 0);
	volatileLiquid = obj.value<fixed>("volatileLiquid", 0);
	volatileIces = obj.value<fixed>("volatileIces", 0);
	volcanicity = obj.value<fixed>("volcanicity", 0);
	atmosOxidizing = obj.value<fixed>("atmosOxidizing", 0);
	atmosDensity = obj.value<double>("atmosDensity", 0);
	atmosColor = obj.value<Color>("atmosColor", 0);
	life = obj.value<fixed>("life", 0);
	population = obj.value<fixed>("population", 0);
	agricultural = obj.value<fixed>("agricultural", 0);

	spaceStationType = obj.value<std::string>("spaceStationType", "");

	heightMapFilename = obj.value<std::string>("heightMapFilename", "");
	heightMapFractal = obj.value<uint32_t>("heightMapFractal", 0);

	want_rand_arg_periapsis = !obj.count("argOfPeriapsis");
	want_rand_offset = !obj.count("orbitalOffset");
	want_rand_phase = !obj.count("orbitalPhase");
	want_rand_seed = !obj.count("seed");
}

// ------- CustomSystem --------

static const char LuaCustomSystem_TypeName[] = "CustomSystem";

static CustomSystem **l_csys_check_ptr(lua_State *L, int idx)
{
	CustomSystem **csptr = static_cast<CustomSystem **>(
		luaL_checkudata(L, idx, LuaCustomSystem_TypeName));
	if (!(*csptr)) {
		abort();
		luaL_error(L, "invalid system (this system has already been used)");
	}
	return csptr;
}

static CustomSystem *l_csys_check(lua_State *L, int idx)
{
	return *l_csys_check_ptr(L, idx);
}

static unsigned interpret_star_types(int *starTypes, lua_State *L, int idx)
{
	LUA_DEBUG_START(L);
	luaL_checktype(L, idx, LUA_TTABLE);
	lua_pushvalue(L, idx);
	unsigned i;
	for (i = 0; i < 4; ++i) {
		int ty = SystemBody::TYPE_GRAVPOINT;
		lua_rawgeti(L, -1, i + 1);
		if (lua_type(L, -1) == LUA_TSTRING) {
			ty = LuaConstants::GetConstantFromArg(L, "BodyType", -1);
			if ((ty < SystemBody::TYPE_STAR_MIN || ty > SystemBody::TYPE_STAR_MAX) && ty != SystemBody::TYPE_GRAVPOINT) {
				luaL_error(L, "system star %d does not have a valid star type", i + 1);
				// unreachable (longjmp in luaL_error)
			}
		} else if (!lua_isnil(L, -1)) {
			luaL_error(L, "system star %d is not a string constant", i + 1);
		}
		lua_pop(L, 1);
		LUA_DEBUG_CHECK(L, 1);

		starTypes[i] = ty;
		if (ty == SystemBody::TYPE_GRAVPOINT) break;
	}
	lua_pop(L, 1);
	LUA_DEBUG_END(L, 0);
	return i;
}

static int l_csys_new(lua_State *L)
{
	const char *name = luaL_checkstring(L, 2);
	int starTypes[4];
	unsigned numStars = interpret_star_types(starTypes, L, 3);

	CustomSystem **csptr = static_cast<CustomSystem **>(
		lua_newuserdata(L, sizeof(CustomSystem *)));
	*csptr = new CustomSystem;
	luaL_setmetatable(L, LuaCustomSystem_TypeName);

	(*csptr)->name = name;
	(*csptr)->numStars = numStars;
	assert(numStars <= 4);
	for (unsigned i = 0; i < numStars; ++i)
		(*csptr)->primaryType[i] = static_cast<SystemBody::BodyType>(starTypes[i]);
	return 1;
}

static int l_csys_seed(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	cs->seed = luaL_checkunsigned(L, 2);
	cs->want_rand_seed = cs->seed == 0;
	lua_settop(L, 1);
	return 1;
}

static int l_csys_explored(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	cs->explored = lua_toboolean(L, 2);
	cs->want_rand_explored = false;
	lua_settop(L, 1);
	return 1;
}

static int l_csys_short_desc(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	cs->shortDesc = luaL_checkstring(L, 2);
	lua_settop(L, 1);
	return 1;
}

static int l_csys_long_desc(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	cs->longDesc = luaL_checkstring(L, 2);
	lua_settop(L, 1);
	return 1;
}

static int l_csys_faction(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);

	std::string factionName = luaL_checkstring(L, 2);
	if (!s_activeCustomSystemsDatabase->GetGalaxy()->GetFactions()->IsInitialized()) {
		s_activeCustomSystemsDatabase->GetGalaxy()->GetFactions()->RegisterCustomSystem(cs, factionName);
		lua_settop(L, 1);
		return 1;
	}

	cs->faction = s_activeCustomSystemsDatabase->GetGalaxy()->GetFactions()->GetFaction(factionName);
	if (cs->faction->idx == Faction::BAD_FACTION_IDX) {
		luaL_argerror(L, 2, "Faction not found");
	}

	lua_settop(L, 1);
	return 1;
}

static int l_csys_other_names(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	std::vector<std::string> other_names;
	if (lua_istable(L, 2)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (lua_isstring(L, -2)) {
				std::string n(lua_tostring(L, -1));
				other_names.push_back(n);
			}
			lua_pop(L, 1); // pop value, keep key for lua_next
		}
		lua_pop(L, 1); // pop table
	}
	cs->other_names = other_names;
	lua_settop(L, 1);
	return 1;
}

static int l_csys_govtype(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	cs->govType = static_cast<Polit::GovType>(LuaConstants::GetConstantFromArg(L, "PolitGovType", 2));
	lua_settop(L, 1);
	return 1;
}

static int l_csys_lawlessness(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	double *value = getDoubleOrFixed(L, 2);
	if (value == nullptr)
		return luaL_error(L, "Bad datatype. Expected fixed or float, got %s", luaL_typename(L, 2));
	cs->lawlessness = fixed::FromDouble(*value);
	cs->want_rand_lawlessness = false;
	lua_settop(L, 1);
	return 1;
}

static void _add_children_to_sbody(lua_State *L, CustomSystemBody *sbody)
{
	lua_checkstack(L, 5); // grow the stack if necessary
	LUA_DEBUG_START(L);
	int i = 0;
	while (true) {
		// first there's a body
		lua_rawgeti(L, -1, ++i);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			break;
		}
		LUA_DEBUG_CHECK(L, 1);
		CustomSystemBody **csptr = l_csb_check_ptr(L, -1);
		CustomSystemBody *kid = *csptr;
		*csptr = 0;
		lua_pop(L, 1);
		LUA_DEBUG_CHECK(L, 0);

		// then there are any number of sub-tables containing direct children
		while (true) {
			lua_rawgeti(L, -1, i + 1);
			LUA_DEBUG_CHECK(L, 1);
			if (!lua_istable(L, -1)) break;
			_add_children_to_sbody(L, kid);
			lua_pop(L, 1);
			LUA_DEBUG_CHECK(L, 0);
			++i;
		}
		lua_pop(L, 1);
		LUA_DEBUG_CHECK(L, 0);

		//Output("add-children-to-body adding %s to %s\n", kid->name.c_str(), sbody->name.c_str());

		sbody->children.push_back(kid);
	}
	//Output("add-children-to-body done for %s\n", sbody->name.c_str());
	LUA_DEBUG_END(L, 0);
}

static unsigned count_stars(CustomSystemBody *csb)
{
	if (!csb)
		return 0;
	unsigned count = 0;
	if (csb->type >= SystemBody::TYPE_STAR_MIN && csb->type <= SystemBody::TYPE_STAR_MAX)
		++count;
	for (CustomSystemBody *child : csb->children)
		count += count_stars(child);
	return count;
}

static int l_csys_bodies(lua_State *L)
{
	CustomSystem *cs = l_csys_check(L, 1);
	CustomSystemBody **primary_ptr = l_csb_check_ptr(L, 2);
	int primary_type = (*primary_ptr)->type;
	luaL_checktype(L, 3, LUA_TTABLE);

	if ((primary_type < SystemBody::TYPE_STAR_MIN || primary_type > SystemBody::TYPE_STAR_MAX) && primary_type != SystemBody::TYPE_GRAVPOINT)
		return luaL_error(L, "first body does not have a valid star type");
	if (primary_type != cs->primaryType[0] && primary_type != SystemBody::TYPE_GRAVPOINT)
		return luaL_error(L, "first body type does not match the system's primary star type");

	lua_pushvalue(L, 3);
	_add_children_to_sbody(L, *primary_ptr);
	lua_pop(L, 1);

	cs->sBody = *primary_ptr;
	*primary_ptr = 0;
	if (cs->sBody) {
		unsigned star_count = count_stars(cs->sBody);
		if (star_count != cs->numStars)
			return luaL_error(L, "expected %u star(s) in system %s, but found %u (did you forget star types in CustomSystem:new?)",
				cs->numStars, cs->name.c_str(), star_count);
		// XXX Someday, we should check the other star types as well, but we do not use them anyway now.
	}

	lua_settop(L, 1);
	return 1;
}

static int l_csys_add_to_sector(lua_State *L)
{
	CustomSystem **csptr = l_csys_check_ptr(L, 1);

	(*csptr)->SanityChecks();

	int x = luaL_checkinteger(L, 2);
	int y = luaL_checkinteger(L, 3);
	int z = luaL_checkinteger(L, 4);
	const vector3d *v = LuaVector::CheckFromLua(L, 5);

	(*csptr)->sectorX = x;
	(*csptr)->sectorY = y;
	(*csptr)->sectorZ = z;
	(*csptr)->pos = vector3f(*v);

	//Output("l_csys_add_to_sector: %s added to %d, %d, %d\n", (*csptr)->name.c_str(), x, y, z);

	s_activeCustomSystemsDatabase->AddCustomSystem(SystemPath(x, y, z), *csptr);
	*csptr = 0;
	return 0;
}

static int l_csys_gc(lua_State *L)
{
	CustomSystem **csptr = static_cast<CustomSystem **>(
		luaL_checkudata(L, 1, LuaCustomSystem_TypeName));
	delete *csptr;
	*csptr = 0;
	return 0;
}

static luaL_Reg LuaCustomSystem_meta[] = {
	{ "new", &l_csys_new },
	{ "seed", &l_csys_seed },
	{ "explored", &l_csys_explored },
	{ "short_desc", &l_csys_short_desc },
	{ "long_desc", &l_csys_long_desc },
	{ "faction", &l_csys_faction },
	{ "govtype", &l_csys_govtype },
	{ "lawlessness", &l_csys_lawlessness },
	{ "bodies", &l_csys_bodies },
	{ "other_names", &l_csys_other_names },
	{ "add_to_sector", &l_csys_add_to_sector },
	{ "__gc", &l_csys_gc },
	{ 0, 0 }
};

void CustomSystem::LoadFromJson(const Json &systemdef)
{
	name = systemdef["name"].get<std::string>();

	if (systemdef.count("otherNames") && systemdef["otherNames"].is_array()) {
		for (const Json &name : systemdef["otherNames"])
			other_names.push_back(name.get<std::string>());
	}

	numStars = systemdef["stars"].size();

	size_t starIdx = 0;
	for (const Json &type : systemdef["stars"]) {
		if (starIdx >= COUNTOF(primaryType))
			break;
		primaryType[starIdx++] = SystemBody::BodyType(EnumStrings::GetValue("BodyType", type.get<std::string>().c_str()));
	}

	sectorX = systemdef["sectorX"];
	sectorY = systemdef["sectorY"];
	sectorZ = systemdef["sectorZ"];

	pos = systemdef["pos"];
	seed = systemdef.value<uint32_t>("seed", 0);
	explored = systemdef.value<bool>("explored", true);
	lawlessness = systemdef.value<fixed>("lawlessness", 0);

	want_rand_seed = !systemdef.count("seed");
	want_rand_explored = !systemdef.count("explored");
	want_rand_lawlessness = !systemdef.count("lawlessness");

	govType = Polit::GovType(EnumStrings::GetValue("PolitGovType", systemdef.value<std::string>("govType", "NONE").c_str()));

	shortDesc = systemdef.value<std::string>("shortDesc", "");
	longDesc = systemdef.value<std::string>("longDesc", "");
}

// NOTE: not currently used, custom systems are initially generated using StarSystem::DumpToJson instead.
void CustomSystem::SaveToJson(Json &obj)
{
	obj["name"] = name;

	if (!other_names.empty()) {
		Json &out_names = obj["otherNames"] = Json::array();
		for (auto &name : other_names)
			out_names.push_back(name);
	}

	Json &out_types = obj["stars"] = Json::array();
	for (size_t idx = 0; idx < numStars; idx++)
		out_types.push_back(EnumStrings::GetString("BodyType", primaryType[idx]));

	obj["numStars"] = numStars;

	obj["sectorX"] = sectorX;
	obj["sectorY"] = sectorY;
	obj["sectorZ"] = sectorZ;

	obj["pos"] = pos;

	if (!want_rand_seed)
		obj["seed"] = seed;
	if (!want_rand_explored)
		obj["explored"] = explored;
	if(!want_rand_lawlessness)
		obj["lawlessness"] = lawlessness;

	obj["govType"] = EnumStrings::GetString("PolitGovType", govType);

	obj["shortDesc"] = shortDesc;
	obj["longDesc"] = longDesc;

}

// ------ CustomSystem initialisation ------

static void register_class(lua_State *L, const char *tname, luaL_Reg *meta)
{
	LUA_DEBUG_START(L);
	luaL_newmetatable(L, tname);
	luaL_setfuncs(L, meta, 0);

	// map the metatable to its own __index
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	// publish the metatable
	lua_setglobal(L, tname);

	LUA_DEBUG_END(L, 0);
}

static void RegisterCustomSystemsAPI(lua_State *L)
{
	register_class(L, LuaCustomSystem_TypeName, LuaCustomSystem_meta);
	register_class(L, LuaCustomSystemBody_TypeName, LuaCustomSystemBody_meta);
}

lua_State *CustomSystemsDatabase::CreateLoaderState()
{
	lua_State *L = luaL_newstate();
	LUA_DEBUG_START(L);

	pi_lua_open_standard_base(L);

	LuaVector::Register(L);
	LuaFixed::Register(L);
	LuaConstants::Register(L);

	// create a shortcut f = fixed.new
	lua_getglobal(L, LuaFixed::LibName);
	lua_getfield(L, -1, "new");
	assert(lua_iscfunction(L, -1));
	lua_setglobal(L, "f");
	lua_pop(L, 1); // pop the fixed table

	// provide shortcut vector constructor: v = vector.new
	lua_getglobal(L, LuaVector::LibName);
	lua_getfield(L, -1, "New");
	assert(lua_iscfunction(L, -1));
	lua_setglobal(L, "v");
	lua_pop(L, 1); // pop the vector table

	LUA_DEBUG_CHECK(L, 0);

	RegisterCustomSystemsAPI(L);

	LUA_DEBUG_END(L, 0);
	return L;
}

void CustomSystemsDatabase::Load()
{
	assert(!s_activeCustomSystemsDatabase);
	s_activeCustomSystemsDatabase = this;
	lua_State *L = CreateLoaderState();
	LUA_DEBUG_START(L);

	pi_lua_dofile_recursive(L, m_customSysDirectory);

	LUA_DEBUG_END(L, 0);
	lua_close(L);
	s_activeCustomSystemsDatabase = nullptr;
}

const CustomSystem *CustomSystemsDatabase::LoadSystem(std::string_view filepath)
{
	assert(!s_activeCustomSystemsDatabase);
	s_activeCustomSystemsDatabase = this;

	m_lastAddedSystem.second = SIZE_MAX;

	lua_State *L = CreateLoaderState();
	LUA_DEBUG_START(L);

	pi_lua_dofile(L, std::string(filepath));

	LUA_DEBUG_END(L, 0);
	lua_close(L);
	s_activeCustomSystemsDatabase = nullptr;

	if (m_lastAddedSystem.second == SIZE_MAX)
		return nullptr;

	return m_sectorMap[m_lastAddedSystem.first][m_lastAddedSystem.second];
}

const CustomSystem *CustomSystemsDatabase::LoadSystemFromJSON(std::string_view filename, const Json &systemdef)
{
	CustomSystem *sys = new CustomSystem();

	try {

		sys->LoadFromJson(systemdef);

		// Validate number of stars
		constexpr int MAX_STARS = COUNTOF(sys->primaryType);
		if (sys->numStars > MAX_STARS) {
			Log::Warning("Custom system {} defines {} stars of {} max! Extra stars will not be used in Sector generation.",
				filename, sys->numStars, MAX_STARS);
			sys->numStars = MAX_STARS;
		}

		// Set system faction pointer
		auto factionName = systemdef.value<std::string>("faction", "");
		if (!factionName.empty()) {
			if (!GetGalaxy()->GetFactions()->IsInitialized()) {
				GetGalaxy()->GetFactions()->RegisterCustomSystem(sys, factionName);
			} else {
				sys->faction = GetGalaxy()->GetFactions()->GetFaction(factionName);
				if (sys->faction->idx == Faction::BAD_FACTION_IDX) {
					Log::Warning("Unknown faction {} for custom system {}.", factionName, filename);
					sys->faction = nullptr;
				}
			}
		}

		size_t numBodies = systemdef["bodies"].size();
		sys->bodies.reserve(numBodies);

		// Load all bodies in order
		for (const Json &bodynode : systemdef["bodies"]) {

			sys->bodies.emplace_back(new CustomSystemBody());

			CustomSystemBody *body = sys->bodies.back();
			body->LoadFromJson(bodynode);

			if (bodynode.count("children")) {

				for (const Json &childIndex : bodynode["children"]) {
					if (childIndex >= numBodies) {
						Log::Warning("Body {} in system {} has out-of-range child index {}",
							body->name, filename, childIndex.get<uint32_t>());
						continue;
					}

					body->childIndicies.push_back(childIndex.get<uint32_t>());
				}

			}

		}

		sys->sBody = sys->bodies[0];

		// Resolve body children pointers
		for (CustomSystemBody *body : sys->bodies) {

			for (uint32_t childIdx : body->childIndicies) {
				body->children.push_back(sys->bodies[childIdx]);
			}

		}

		SystemPath path(sys->sectorX, sys->sectorY, sys->sectorZ, 0, 0);
		AddCustomSystem(path, sys);

		return sys;

	} catch (Json::out_of_range &e) {
		Log::Warning("Could not load JSON system definition {}!", filename);

		delete sys;
		return nullptr;
	}
}

CustomSystemsDatabase::~CustomSystemsDatabase()
{
	for (SectorMap::iterator secIt = m_sectorMap.begin(); secIt != m_sectorMap.end(); ++secIt) {
		for (CustomSystemsDatabase::SystemList::iterator sysIt = secIt->second.begin();
			 sysIt != secIt->second.end(); ++sysIt) {
			delete *sysIt;
		}
	}
	m_sectorMap.clear();
}

const CustomSystemsDatabase::SystemList &CustomSystemsDatabase::GetCustomSystemsForSector(int x, int y, int z) const
{
	SystemPath path(x, y, z);
	SectorMap::const_iterator it = m_sectorMap.find(path);
	return (it != m_sectorMap.end()) ? it->second : s_emptySystemList;
}

void CustomSystemsDatabase::AddCustomSystem(const SystemPath &path, CustomSystem *csys)
{
	csys->systemIndex = m_sectorMap[path].size();
	m_lastAddedSystem = SystemIndex(path, csys->systemIndex);
	m_sectorMap[path].push_back(csys);
}

CustomSystem::CustomSystem() :
	sBody(nullptr),
	numStars(0),
	seed(0),
	want_rand_seed(true),
	want_rand_explored(true),
	faction(nullptr),
	govType(Polit::GOV_INVALID),
	want_rand_lawlessness(true)
{
	for (int i = 0; i < 4; ++i)
		primaryType[i] = SystemBody::TYPE_GRAVPOINT;
}

CustomSystem::~CustomSystem()
{
	delete sBody;
}

void CustomSystem::SanityChecks()
{
	if (IsRandom())
		return;
	else
		sBody->SanityChecks();
}

CustomSystemBody::CustomSystemBody() :
	aspectRatio(fixed(1, 1)),
	averageTemp(1),
	want_rand_offset(true),
	want_rand_arg_periapsis(true),
	want_rand_phase(true),
	latitude(0.0),
	longitude(0.0),
	volatileGas(0),
	ringStatus(WANT_RANDOM_RINGS),
	seed(0),
	want_rand_seed(true)
{
}

CustomSystemBody::~CustomSystemBody()
{
	for (std::vector<CustomSystemBody *>::iterator
			 it = children.begin();
		 it != children.end(); ++it) {
		delete (*it);
	}
}

static void checks(CustomSystemBody &csb)
{
	if (csb.name.empty()) {
		Error("custom system with name not set!\n");
		// throw an exception? Then it can be "catch" *per file*...
	}
	if (csb.radius <= 0 && csb.mass <= 0) {
		if (csb.type != SystemBody::TYPE_STARPORT_ORBITAL &&
			csb.type != SystemBody::TYPE_STARPORT_SURFACE &&
			csb.type != SystemBody::TYPE_GRAVPOINT) Error("custom system body '%s' with both radius ans mass left undefined!", csb.name.c_str());
	}
	if (csb.radius <= 0 && csb.type != SystemBody::TYPE_STARPORT_ORBITAL &&
		csb.type != SystemBody::TYPE_STARPORT_SURFACE &&
		csb.type != SystemBody::TYPE_GRAVPOINT) {
		Output("Warning: 'radius' is %f for body '%s'\n", csb.radius.ToFloat(), csb.name.c_str());
	}
	if (csb.mass <= 0 && csb.type != SystemBody::TYPE_STARPORT_ORBITAL &&
		csb.type != SystemBody::TYPE_STARPORT_SURFACE &&
		csb.type != SystemBody::TYPE_GRAVPOINT) {
		Output("Warning: 'mass' is %f for body '%s'\n", csb.mass.ToFloat(), csb.name.c_str());
	}
	if (csb.averageTemp <= 0 && csb.type != SystemBody::TYPE_STARPORT_ORBITAL &&
		csb.type != SystemBody::TYPE_STARPORT_SURFACE &&
		csb.type != SystemBody::TYPE_GRAVPOINT) {
		Output("Warning: 'averageTemp' is %i for body '%s'\n", csb.averageTemp, csb.name.c_str());
	}
	if (csb.type == SystemBody::TYPE_STAR_S_BH ||
		csb.type == SystemBody::TYPE_STAR_IM_BH ||
		csb.type == SystemBody::TYPE_STAR_SM_BH) {
		double schwarzschild = 2 * csb.mass.ToDouble() * ((G * SOL_MASS) / (LIGHT_SPEED * LIGHT_SPEED));
		schwarzschild /= SOL_RADIUS;
		if (csb.radius < schwarzschild) {
			Output("Warning: Blackhole radius defaulted to Schwarzschild radius (%f Sol radii)\n", schwarzschild);
			csb.radius = schwarzschild;
		}
	}
}

void CustomSystemBody::SanityChecks()
{
	checks(*this);
	for (CustomSystemBody *csb : children)
		csb->SanityChecks();
}
