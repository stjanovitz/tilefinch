/* Constraint validation's checks and messages: which ValidityState flags a
   form control raises and the message validationMessage reports. Built the
   first time a page reads a control's validity or validationMessage or
   asks it to check itself, so a realm that never validates a form pays
   nothing for them. platform.js passes the realm values and control state
   it captured before page script ran; everything else here is read when a
   check runs, exactly as it was when this lived in platform.js. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateConstraintValidation", {
  __proto__: null,
  configurable: true,
  value({
    String,
    Number,
    HTMLTextAreaElement,
    textValueTypes,
    numberValueTypes,
    inputNumber,
    constraintNumber,
    controlStateHas,
    userEditedValues,
    isChecked,
    radioPeers,
  }) {
    const htmlEmailPattern =
        /^[a-z0-9.!#$%&'*+\/=?^_`{|}~-]+@[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*$/i,
      controlValidity = (control, only = "valid") => {
        const value = String(control.value || ""),
          type = String(control.type || "text").toLowerCase(),
          all = only === "valid",
          required = (all || only === "valueMissing") && control.required;
        let valueMissing = false;
        if (!all && only !== "valueMissing") {
        } else if (type === "radio") {
          if (!isChecked(control)) {
            let groupRequired = required, groupChecked = false;
            for (const item of radioPeers(control)) {
              if (isChecked(item)) { groupChecked = true; break; }
              if (item.required) groupRequired = true;
            }
            valueMissing = groupRequired && !groupChecked;
          }
        } else if (required) {
          if (type === "checkbox") valueMissing = !control.checked;
          else valueMissing = value === "";
        }
        let patternMismatch = false;
        const pattern = all || only === "patternMismatch" ? control.getAttribute("pattern") : null;
        if (value && pattern !== null && textValueTypes.has(type)) {
          try {
            const regex = new RegExp("^(?:" + pattern + ")$", "u");
            patternMismatch = type === "email" && control.multiple
              ? value.split(",").some(address => !regex.test(address))
              : !regex.test(value);
          } catch (_) {}
        }
        const checkLength = (all || only === "tooShort" || only === "tooLong") &&
            (control instanceof HTMLTextAreaElement || textValueTypes.has(type)) &&
            controlStateHas(userEditedValues, control),
          minimumLength = checkLength ? control.minLength : -1,
          maximumLength = checkLength ? control.maxLength : -1,
          tooShort =
            checkLength && value !== "" && minimumLength >= 0 && value.length < minimumLength,
          tooLong = checkLength && maximumLength >= 0 && value.length > maximumLength;
        let typeMismatch = false;
        if (!all && only !== "typeMismatch") {
        } else if (value && type === "email") {
          typeMismatch = control.multiple
            ? value.split(",").some(address => !htmlEmailPattern.test(address))
            : !htmlEmailPattern.test(value);
        }
        else if (value && type === "url") {
          try {
            new URL(value);
          } catch (_) {
            typeMismatch = true;
          }
        }
        const numeric = value && numberValueTypes.has(type) && (all ||
            only === "rangeUnderflow" || only === "rangeOverflow" || only === "stepMismatch")
            ? constraintNumber(type, value) : NaN,
          bounded = Number.isFinite(numeric),
          minimum = bounded ? constraintNumber(type, control.getAttribute("min")) : NaN,
          maximum = bounded ? constraintNumber(type, control.getAttribute("max")) : NaN,
          hasMinimum = Number.isFinite(minimum),
          hasMaximum = Number.isFinite(maximum),
          periodic = type === "time" && hasMinimum && hasMaximum && maximum < minimum,
          periodicGap = periodic && numeric > maximum && numeric < minimum,
          rangeUnderflow = periodic ? periodicGap : hasMinimum && numeric < minimum,
          rangeOverflow = periodic ? periodicGap : hasMaximum && numeric > maximum;
        let stepMismatch = false;
        const stepUnit = type === "date" ? 86400000 : type === "week" ? 604800000
          : type === "time" || type === "datetime-local" ? 1000 : 1,
          rawStep = bounded ? control.getAttribute("step") : null,
          parsedStep = inputNumber(rawStep),
          defaultStep = type === "time" || type === "datetime-local" ? 60 : 1,
          step = !bounded || rawStep?.toLowerCase() === "any" ? NaN
            : (parsedStep > 0 ? parsedStep : defaultStep) * stepUnit;
        if (
          value !== "" &&
          Number.isFinite(numeric) &&
          Number.isFinite(step) &&
          step > 0
        ) {
          const valueAttribute = constraintNumber(type, control.getAttribute("value")),
            base = hasMinimum ? minimum : Number.isFinite(valueAttribute) ? valueAttribute
              : type === "week" ? -259200000 : 0;
          stepMismatch =
            Math.abs(
              (numeric - base) / step - Math.round((numeric - base) / step),
            ) > 1e-9;
        }
        const customError = !!control.__customValidity,
          badInput = false,
          valid = !(
            valueMissing ||
            patternMismatch ||
            tooShort ||
            tooLong ||
            typeMismatch ||
            rangeUnderflow ||
            rangeOverflow ||
            stepMismatch ||
            customError ||
            badInput
          );
        return {
          valueMissing,
          typeMismatch,
          patternMismatch,
          tooLong,
          tooShort,
          rangeUnderflow,
          rangeOverflow,
          stepMismatch,
          badInput,
          customError,
          valid,
        };
      };
    return {
      controlValidity,
      validationMessage: (control) => {
        const validity = controlValidity(control);
        if (validity.valid) return "";
        if (validity.customError) return control.__customValidity;
        return validity.valueMissing
          ? "Please fill out this field."
          : validity.typeMismatch
            ? "Please enter a valid value."
            : validity.patternMismatch
              ? "Please match the requested format."
              : validity.tooShort
                ? "Please lengthen this text."
                : validity.tooLong
                  ? "Please shorten this text."
                  : validity.rangeUnderflow
                    ? "Value is below the minimum."
                    : validity.rangeOverflow
                      ? "Value is above the maximum."
                      : validity.stepMismatch
                        ? "Please enter a valid step value."
                        : "Invalid value.";
      },
    };
  },
});
