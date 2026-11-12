/* CSSOM rule objects: CSSRule and its subclasses. An on-demand module,
   evaluated the first time a page reads a sheet's cssRules or names one of
   the rule interfaces; custom-elements.js owns the accessors and the sheet.
   A rule keeps its text; style rules, keyframes and @font-face expose their
   block as a declaration, and grouping rules list their children. A write
   through a rule rebuilds that rule's text and its ancestors' and replaces
   the top-level rule in the sheet, keeping every rule object; a refused
   write leaves every rule object as it was. */
Object.defineProperty(globalThis, "__tilefinchCreateRuleModel", {
  configurable: true,
  value(ruleToken, makeTextStyle, splitRules, textLimit, ruleList,
        validRuleText, assertSelector) {
    const quotaError = () =>
        new DOMException("Stylesheet exceeds bounded size", "QuotaExceededError"),
      mediaList = (text) => {
        const items = text ? text.split(/\s*,\s*/) : [];
        return ruleList(Object.assign(items, {
          mediaText: text,
          toString: () => text,
        }));
      };
    class CSSRule {
      constructor(token) {
        if (token !== ruleToken) throw new TypeError("Illegal constructor");
      }
      get cssText() { return this.__text; }
      set cssText(_) {}
      get parentRule() { return this.__parent; }
      get parentStyleSheet() { return this.__sheet; }
      get type() { return this.__type; }
      /* Rebuilds this rule's text and its ancestors' and pushes the
         top-level rule to the sheet. Growth past the sheet's text bound is
         refused as the sheet's own insertRule refuses it, whether or not the
         sheet is adopted. On any refusal every text is restored and `undo`
         puts back the caller's own change, so the rule objects keep
         matching the applied sheet. */
      __changed(undo) {
        const saved = [];
        let rule = this;
        try {
          for (;; rule = rule.__parent) {
            saved.push(rule, rule.__text);
            rule.__text = rule.__serialize();
            if (!rule.__parent) break;
          }
          const sheet = rule.__sheet;
          if (
            sheet?.__cssRules.includes(rule) &&
            rule.__text.length - saved[saved.length - 1].length >
              textLimit - sheet.__textLength
          )
            throw quotaError();
          sheet?.__replaceRule(rule);
        } catch (error) {
          for (let at = 0; at < saved.length; at += 2)
            saved[at].__text = saved[at + 1];
          undo();
          throw error;
        }
      }
      /* The block's declaration, made on first read of `style`. */
      __declaration() {
        return this.__style ||= makeTextStyle(() => this.__block, (text) => {
          const old = this.__block;
          this.__block = text;
          this.__changed(() => { this.__block = old; });
        });
      }
      __blockText() {
        const block = String(this.__block).trim();
        return block ? " " + block + (block.endsWith(";") ? "" : ";") + " "
          : " ";
      }
    }
    const types = [
      "STYLE_RULE", 1, "CHARSET_RULE", 2, "IMPORT_RULE", 3, "MEDIA_RULE", 4,
      "FONT_FACE_RULE", 5, "PAGE_RULE", 6, "KEYFRAMES_RULE", 7,
      "KEYFRAME_RULE", 8, "NAMESPACE_RULE", 10, "COUNTER_STYLE_RULE", 11,
      "SUPPORTS_RULE", 12, "FONT_FEATURE_VALUES_RULE", 14,
    ];
    for (let at = 0; at < types.length; at += 2)
      for (const target of [CSSRule, CSSRule.prototype])
        Object.defineProperty(target, types[at], {
          enumerable: true,
          value: types[at + 1],
        });
    class CSSStyleRule extends CSSRule {
      get selectorText() { return this.__selector; }
      set selectorText(value) {
        value = String(value).trim();
        try {
          document.documentElement.matches(value);
        } catch (_) {
          return;
        }
        const old = this.__selector;
        this.__selector = value;
        this.__changed(() => { this.__selector = old; });
      }
      get style() { return this.__declaration(); }
      __serialize() {
        return this.__selector + " {" + this.__blockText() + "}";
      }
    }
    class CSSGroupingRule extends CSSRule {
      get cssRules() { return this.__rules; }
      insertRule(rule, index = 0) {
        index = Number(index);
        if (!Number.isInteger(index) || index < 0 || index > this.__rules.length)
          throw new DOMException("Invalid rule index", "IndexSizeError");
        const split = splitRules(String(rule), textLimit),
          parsed = split.rules;
        if (split.truncated) throw quotaError();
        if (parsed.length !== 1 || !validRuleText(parsed[0]))
          throw new DOMException("Invalid CSS rule", "SyntaxError");
        this.__rules.splice(index, 0,
          parseRule(parsed[0], this.__sheet, this));
        this.__changed(() => this.__rules.splice(index, 1));
        return index;
      }
      deleteRule(index) {
        index = Number(index);
        if (!Number.isInteger(index) || index < 0 || index >= this.__rules.length)
          throw new DOMException("Invalid rule index", "IndexSizeError");
        const removed = this.__rules.splice(index, 1)[0];
        this.__changed(() => this.__rules.splice(index, 0, removed));
      }
      __serialize() {
        return "@" + this.__name + (this.__prelude ? " " + this.__prelude : "") +
          " {\n" + this.__rules.map((rule) => "  " + rule.__text).join("\n") +
          "\n}";
      }
    }
    class CSSConditionRule extends CSSGroupingRule {
      get conditionText() { return this.__prelude; }
    }
    class CSSMediaRule extends CSSConditionRule {
      get media() { return this.__media ||= mediaList(this.__prelude); }
    }
    class CSSSupportsRule extends CSSConditionRule {}
    class CSSImportRule extends CSSRule {
      get href() {
        const match = /^(?:url\(\s*)?(["']?)([^"')\s]*)\1/.exec(this.__prelude);
        return match ? match[2] : "";
      }
      get media() {
        return this.__media ||= mediaList(
          this.__prelude.replace(/^(?:url\([^)]*\)|"[^"]*"|'[^']*'|\S+)\s*/, ""));
      }
      get styleSheet() { return null; }
    }
    class CSSFontFaceRule extends CSSRule {
      get style() { return this.__declaration(); }
      __serialize() { return "@font-face {" + this.__blockText() + "}"; }
    }
    class CSSKeyframesRule extends CSSRule {
      get name() { return this.__prelude; }
      get cssRules() { return this.__rules; }
      findRule(key) {
        key = String(key).trim().toLowerCase();
        return this.__rules.find((rule) => rule.keyText === key) ?? null;
      }
    }
    CSSKeyframesRule.prototype.__serialize =
      CSSGroupingRule.prototype.__serialize;
    class CSSKeyframeRule extends CSSRule {
      get keyText() { return this.__selector; }
      get style() { return this.__declaration(); }
      __serialize() {
        return this.__selector + " {" + this.__blockText() + "}";
      }
    }
    const model = {
      CSSRule, CSSStyleRule, CSSGroupingRule, CSSConditionRule, CSSMediaRule,
      CSSSupportsRule, CSSImportRule, CSSFontFaceRule, CSSKeyframesRule,
      CSSKeyframeRule,
    };
    const parseRule = (text, sheet, parent) => {
      const source = text.trim(),
        open = source.indexOf("{"),
        block = open < 0 ? "" : source.slice(open + 1, source.lastIndexOf("}")),
        at = source[0] === "@",
        name = at ? (/^@([-\w]+)/.exec(source)?.[1] || "").toLowerCase() : "",
        head = (open < 0 ? source.replace(/;\s*$/, "") : source.slice(0, open))
          .slice(at ? name.length + 1 : 0).trim(),
        keyframe = parent instanceof model.CSSKeyframesRule,
        keyframes = /^(?:-webkit-)?keyframes$/.test(name),
        type = keyframe ? 8 : !at ? 1 : name === "media" ? 4
          : name === "supports" ? 12 : name === "import" ? 3
          : name === "font-face" ? 5 : keyframes ? 7 : 0,
        Rule = type === 8 ? model.CSSKeyframeRule
          : type === 1 ? model.CSSStyleRule
          : type === 4 ? model.CSSMediaRule
          : type === 12 ? model.CSSSupportsRule
          : type === 3 ? model.CSSImportRule
          : type === 5 ? model.CSSFontFaceRule
          : type === 7 ? model.CSSKeyframesRule
          : open >= 0 ? model.CSSGroupingRule : model.CSSRule,
        rule = new Rule(ruleToken);
      rule.__text = text;
      rule.__sheet = sheet;
      rule.__parent = parent || null;
      rule.__type = type;
      rule.__name = name;
      rule.__prelude = head;
      rule.__block = block;
      if (type === 1 || type === 8)
        rule.__selector = keyframe ? head.toLowerCase() : head;
      else if (type === 4 || type === 7 || type === 12 ||
               Rule === model.CSSGroupingRule)
        rule.__rules = ruleList(splitRules(block,
          textLimit).rules.map((child) =>
            parseRule(child, sheet, rule)));
      return rule;
    };
    return { model, parseRule };
  },
});
