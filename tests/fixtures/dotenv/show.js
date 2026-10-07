for (const k of ["PLAIN","SINGLE","DOUBLE","MULTI","URL","FROM_LOCAL","DEFAULTED","EXPAND","ESCAPED","EMPTY","MODE","KEEP"]) console.log(k, JSON.stringify(process.env[k]));
